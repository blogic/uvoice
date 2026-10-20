// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Unit tests for the call legs (src/proto/sip/call.c).
 *
 * The suite uses the scripted environment of test_sip_txn.c. The test
 * advances a virtual clock by hand, a capture array replaces the socket, and
 * a timer table replaces uloop. The fixture wires one transaction manager,
 * one UA core and one dialog set exactly as account.c wires them. The router
 * below mirrors the account interface of call.c. call.c therefore cannot tell
 * the fixture from the daemon.
 *
 * The account is fictitious. No provider credential appears in this file.
 */
#include <arpa/inet.h>
#include <ctype.h>
#include <stdlib.h>
#include <unistd.h>

#include <libubox/blobmsg.h>
#include <libubox/ulog.h>

#include "proto/sip/account.h"
#include "proto/sip/call.h"
#include "net/ports.h"
#include "proto/shared/media_fill.h"
#include "proto/sip/dialog.h"
#include "proto/shared/sdp.h"
#include "proto/sip/sub.h"
#include "proto/sip/ua.h"
#include "proto_host_fake.h"
#include "test_msg.h"
#include "test_util.h"
#include "test_warn.h"
#include "uvoice.h"

#define FAKE_TIMER_MAX	256
#define FAKE_SEND_MAX	64
#define WIRE_MAX	4096
#define TEXT_MAX	4096

#define T_USER		"1001"
#define T_USER2		"2001"		/* the second line on the same socket */
#define T_PASS		"not-a-real-secret"
#define T_DOMAIN	"sip.example.net"
#define T_HOST		"10.0.0.5"
#define T_PORT		5060
#define T_PEER		"198.51.100.7"
#define T_PEER_PORT	5060
#define T_NONCE		"1f3870be274f6c49b3e31a0c6728957f"
#define T_CONTACT	"Contact: <sip:1002@198.51.100.7:5060>\r\n"
/*
 * The IPv6 documentation prefix (RFC 3849). T_HOST6 is this line's own media
 * and signalling address, T_PEER6 is the far end, and T_DOMAIN6 is a domain
 * the account layer already bracketed, which is the form the view hands down.
 */
#define T_HOST6		"2001:db8::7"
#define T_PEER6		"2001:db8::9"
#define T_DOMAIN6	"[2001:db8::1]"

/* the service route of a registrar, as it arrives and as it is re-emitted */
#define T_SR_HDRS	"Service-Route: <sip:p1.example.net;lr>\r\n" \
			"Service-Route: <sip:p2.example.net;lr>\r\n"
#define T_SR_TEXT	"Route: <sip:p1.example.net;lr>\r\n" \
			"Route: <sip:p2.example.net;lr>\r\n"
/* a hop on a host the line never resolves and never sends to */
#define T_SR_OTHER	"Service-Route: <sip:far.example.org:5070;lr>\r\n"

struct fake_send {
	char buf[WIRE_MAX + 1];
	size_t len;
	char host[64];
	uint16_t port;
};

static struct {
	struct sip_txn_env env;
	uint64_t now;

	struct sip_timer *timer[FAKE_TIMER_MAX];
	unsigned int timers;
	unsigned int fired;

	struct fake_send send[FAKE_SEND_MAX];
	unsigned int sends;
	bool send_fail;

	struct sip_txn_dst peer;
	/*
	 * The daemon HAL state, driven exactly as main.c drives it: the shell
	 * hands the leg layer a predicate over this value (call.h
	 * calls_hal_ready_bind) and the admission rule reads the answer.
	 */
	enum uvoice_hal_state hal_state;
	/* the service state of each line, the second half of the admission
	 * rule (call.h calls_line_admit_bind) */
	bool line_oos[CFG_CHANNEL_MAX];
	bool view_ok;
	bool dst_valid;
	bool registered;
	bool active;
	bool no_contact;		/* responses carry no Contact */
	/* the view's own address, which is the family this line runs in */
	const char *local_host;
	/* the view's domain, bracketed by the account layer when it is IPv6 */
	const char *domain;
	const char *transport;		/* the view's transport token, NULL = UDP */
	/* the view's resolved netdev, NULL = the account scoped nothing */
	const char *bind_dev;
	/* the service route the view exposes, NULL when the account exposes
	 * none (account.h struct acct_call_view) */
	const struct sip_route_hdrs *service_route;
	unsigned int line;		/* the socket owner of the fixture */
	unsigned int mwi_notifies;	/* what the account intake consumed */

	/* which line a Call-ID belongs to, the fixture's dialog index */
	struct {
		char call_id[160];
		unsigned int line;
		bool used;
	} route[4];
	unsigned int routes;
} fk;

static struct sip_txn_mgr mgr;
static struct sip_dialog_set dlgs;
static struct ua ua;
static struct cfg cfg;

#include "test_capture.h"
#include "test_clock.h"

/* ------------------------------------------------------------ environment */

/*
 * The flash relay trace, which is the only report a refused relay makes: the
 * wire carries nothing and no callback fires. Every layer under the fixture
 * writes this one callback, so the recorder keeps the interesting lines alone
 * instead of the last line of all of them. The ACK line is the second one: a
 * 2xx whose ACK never left says so there and nowhere else.
 * test_reinvite_out_uas() reads it for our re-INVITE in a peer dialog.
 */
static struct {
	char flash[256];
	unsigned int flash_lines;
	unsigned int ack_stall_lines;
	char alert[256];
	unsigned int alert_lines;
	/* the two reports of the SRTP leg: the plain fallback of a leg with no
	 * entropy op, and the refused T.38 upspeed */
	unsigned int srtp_plain_lines;
	unsigned int srtp_t38_lines;
	/* the two refuse-not-truncate reports of the in-dialog send: the extra
	 * header block did not fit its own buffer, or the request did not fit
	 * the wire buffer (call.h CALL_WIRE_EXTRA_MAX) */
	unsigned int extra_refused_lines;
	unsigned int wire_refused_lines;
	/*
	 * The RFC 3262 reports of the UAC side. Every one of them is read by an
	 * end to end flow, so a reworded line fails a case here and not a flow
	 * later: the PRACK that went out or did not, the two discards, the
	 * reliable 1xx a PRACK cannot answer, the answer a reliable 1xx closed
	 * the exchange with, and the two failures the INVITE survives.
	 */
	unsigned int prack_sent_lines;
	unsigned int prack_unsent_lines;
	unsigned int prack_retx_lines;
	unsigned int prack_order_lines;
	unsigned int prack_no_rseq_lines;
	unsigned int prack_no_dialog_lines;
	unsigned int prack_answer_lines;
	unsigned int prack_body_lines;
	unsigned int prack_refused_lines;
	unsigned int prack_dead_lines;
	/*
	 * The end of an INVITE server transaction the transport refused. It
	 * belongs to every inbound leg and not to RFC 3262: a plain 180 the
	 * peer asks for again draws it as a reliable one does.
	 */
	unsigned int ist_fail_lines;
	/*
	 * The RFC 3262 reports of the UAS side, counted for the same reason:
	 * the reliable 180, each copy of it, the give-up of its schedule, and
	 * the two answers an inbound PRACK draws.
	 */
	unsigned int rel_sent_lines;
	unsigned int rel_retx_lines;
	unsigned int rel_giveup_lines;
	unsigned int rel_match_lines;
	unsigned int rel_nomatch_lines;
} tr;

static void fake_trace(struct proto_timer_env *env, const char *msg)
{
	if (!msg)
		return;

	if (strstr(msg, "the ACK could not be"))
		tr.ack_stall_lines++;

	/* the one report an Alert-Info that named no ring template makes */
	if (strstr(msg, "default ring")) {
		snprintf(tr.alert, sizeof(tr.alert), "%s", msg);
		tr.alert_lines++;
	}

	if (strstr(msg, "offers plain RTP"))
		tr.srtp_plain_lines++;

	if (strstr(msg, "T.38 upspeed refused on an SRTP leg"))
		tr.srtp_t38_lines++;

	if (strstr(msg, "header block does not fit"))
		tr.extra_refused_lines++;

	if (strstr(msg, "does not fit the wire buffer"))
		tr.wire_refused_lines++;

	if (strstr(msg, ", PRACK sent"))
		tr.prack_sent_lines++;

	if (strstr(msg, ", PRACK not sent"))
		tr.prack_unsent_lines++;

	if (strstr(msg, "retransmitted reliable 1xx"))
		tr.prack_retx_lines++;

	if (strstr(msg, "out of order, ignored"))
		tr.prack_order_lines++;

	if (strstr(msg, "without a usable RSeq"))
		tr.prack_no_rseq_lines++;

	if (strstr(msg, "without a dialog, treated as plain"))
		tr.prack_no_dialog_lines++;

	if (strstr(msg, "reliable 1xx answer taken"))
		tr.prack_answer_lines++;

	if (strstr(msg, "2xx body ignored"))
		tr.prack_body_lines++;

	if (strstr(msg, "PRACK answered"))
		tr.prack_refused_lines++;

	if (strstr(msg, "PRACK unanswered"))
		tr.prack_dead_lines++;

	if (strstr(msg, "180 sent reliably"))
		tr.rel_sent_lines++;

	if (strstr(msg, "180 retransmitted"))
		tr.rel_retx_lines++;

	if (strstr(msg, "INVITE server transaction failed"))
		tr.ist_fail_lines++;

	if (strstr(msg, "no PRACK within 64*T1"))
		tr.rel_giveup_lines++;

	if (strstr(msg, "PRACK matched RSeq"))
		tr.rel_match_lines++;

	if (strstr(msg, "PRACK matches no reliable provisional"))
		tr.rel_nomatch_lines++;

	if (!strstr(msg, "flash INFO"))
		return;

	snprintf(tr.flash, sizeof(tr.flash), "%s", msg);
	tr.flash_lines++;
}

/* the armed delay of one timer id, 0 when none is armed */
static uint64_t timer_delay(enum sip_timer_id id)
{
	unsigned int i;

	for (i = 0; i < fk.timers; i++) {
		if (fk.timer[i]->id == (uint8_t)id)
			return fk.timer[i]->due_ms - fk.now;
	}

	return 0;
}

static bool timer_armed(enum sip_timer_id id)
{
	unsigned int i;

	for (i = 0; i < fk.timers; i++) {
		if (fk.timer[i]->id == (uint8_t)id)
			return true;
	}

	return false;
}

/*
 * One timer fired short of the time it was armed for. uloop runs a timeout as
 * soon as the interval left truncates to zero milliseconds, so the callback
 * can read a clock that is still under its own deadline; fake_advance() moves
 * the clock to the due time first and never shows that skew.
 */
static void timer_fire_early(enum sip_timer_id id, uint64_t ms)
{
	unsigned int i;

	for (i = 0; i < fk.timers; i++) {
		struct sip_timer *t = fk.timer[i];

		if (t->id != (uint8_t)id)
			continue;

		fk.now = t->due_ms - ms;
		fake_timer_drop(t);
		sip_timer_expire(t);
		return;
	}
}

/*
 * The three helpers that name a timer by its object. The transfer guard is
 * armed on the timer id of the setup bound and inherits its value (call.h:
 * the wait is timer C, and the sip/txn.h interface is fixed), so an id alone
 * cannot tell the two apart. A case therefore counts the timers that carry
 * that id, takes the object while exactly one does, and asks about that
 * pointer afterwards: no other timer of this layer can be it.
 */
static unsigned int timer_count(enum sip_timer_id id)
{
	unsigned int i, n = 0;

	for (i = 0; i < fk.timers; i++) {
		if (fk.timer[i]->id == (uint8_t)id)
			n++;
	}

	return n;
}

static const struct sip_timer *timer_obj(enum sip_timer_id id)
{
	unsigned int i;

	for (i = 0; i < fk.timers; i++) {
		if (fk.timer[i]->id == (uint8_t)id)
			return fk.timer[i];
	}

	return NULL;
}

/* the n-th timer that carries an id, in arm order: a consult transfer arms
 * two guards on the id of the setup bound, the drain guard first */
static const struct sip_timer *timer_obj_nth(enum sip_timer_id id, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < fk.timers; i++) {
		if (fk.timer[i]->id != (uint8_t)id)
			continue;
		if (!n--)
			return fk.timer[i];
	}

	return NULL;
}

/* the armed delay of one timer object, 0 when it is not armed */
static uint64_t timer_delay_obj(const struct sip_timer *t)
{
	unsigned int i;

	for (i = 0; i < fk.timers; i++) {
		if (fk.timer[i] == t)
			return t->due_ms - fk.now;
	}

	return 0;
}

static bool timer_obj_armed(const struct sip_timer *t)
{
	unsigned int i;

	for (i = 0; i < fk.timers; i++) {
		if (fk.timer[i] == t)
			return true;
	}

	return false;
}

/* ------------------------------------------ the account_call_view() fixture */

static void fake_ack_missing(struct ua *u, uint32_t owner, void *priv)
{
	if (acct_owner_kind_of(owner) == ACCT_OWNER_CALL)
		call_ua_ack_missing(acct_owner_val(owner));
}

static void fake_glare_retry(struct ua *u, uint32_t owner, void *priv)
{
	if (acct_owner_kind_of(owner) == ACCT_OWNER_CALL)
		call_ua_glare_retry(acct_owner_val(owner));
}

static void fake_retry_after(struct ua *u, uint32_t owner, uint32_t secs, bool present,
			     void *priv)
{
}

static const struct ua_cb ua_cb = {
	.ack_missing = fake_ack_missing,
	.retry_after = fake_retry_after,
	.glare_retry = fake_glare_retry,
};

static void fake_resp(struct sip_txn_env *env, uint32_t owner, uint32_t txn,
		      const struct sip_msg *msg)
{
	if (acct_owner_kind_of(owner) == ACCT_OWNER_CALL)
		call_txn_resp(acct_owner_val(owner), txn, msg);
}

static void fake_done(struct sip_txn_env *env, uint32_t owner, uint32_t txn,
		      enum sip_txn_reason reason)
{
	if (acct_owner_kind_of(owner) == ACCT_OWNER_CALL)
		call_txn_done(acct_owner_val(owner), txn, reason);
}

/* the two lines share one socket, one manager and one dialog set */
static const char *line_user(unsigned int line)
{
	return line ? T_USER2 : T_USER;
}

bool account_call_view(unsigned int line, struct acct_call_view *out)
{
	if (line >= CFG_CHANNEL_MAX || !out || !fk.view_ok)
		return false;

	memset(out, 0, sizeof(*out));
	out->mgr = &mgr;
	out->env = &fk.env;
	out->ua = &ua;
	out->dialogs = &dlgs;
	out->local_host = fk.local_host;
	out->domain = fk.domain;
	out->user = line_user(line);
	out->display = "Test Line";
	out->auth_user = T_USER;
	out->auth_pass = T_PASS;
	out->user_agent = "uvoice-test/1";
	out->transport = fk.transport;
	out->bind_dev = fk.bind_dev;
	out->local_port = T_PORT;
	out->dst = &fk.peer;
	out->dst_valid = fk.dst_valid;
	out->registered = fk.registered;
	out->service_route = fk.service_route;
	out->active = fk.active;

	return true;
}

/*
 * The shell's admission predicate (line/line.c line_hal_ready, bound through
 * call.h calls_hal_ready_bind). It is copied here rather than approximated,
 * so a case names the daemon HAL state it drives and the leg layer sees
 * exactly what it sees in the daemon.
 */
static bool fake_hal_ready(void)
{
	return fk.hal_state == UVOICE_HAL_READY;
}

/*
 * The shell's service predicate (line/line.c line_call_admit, bound through
 * call.h calls_line_admit_bind). A line the shell holds out of service takes no
 * inbound call, and the fixture drives that state per line.
 */
static bool fake_line_admit(unsigned int line)
{
	return line < CFG_CHANNEL_MAX && !fk.line_oos[line];
}

/* --------------------------------------------------------- media recorder */

/*
 * The order of the media ops is part of the contract. The media numbers can
 * only be read while the stream still runs, so the recorder keeps the
 * sequence and a case asserts that the snapshot stands in front of the stop
 * instead of merely counting both.
 */
#define MED_SEQ_MAX	16

enum med_op {
	MED_CONFIG = 0,
	MED_START,
	MED_STOP,
	MED_DTMF,
	MED_SNAPSHOT,
	MED_FAX,
};

static struct {
	unsigned int config_calls;
	/*
	 * Configurations whose media carried no hold silence. med.last holds
	 * only the last description, so a case that must say "no configuration
	 * of this whole phase opened the uplink" counts them instead. A leg
	 * that was silent before and after but live for one exchange in between
	 * is the defect this counter exists to catch.
	 */
	unsigned int config_live;
	unsigned int start_calls;
	unsigned int stop_calls;
	unsigned int dtmf_calls;
	unsigned int snapshot_calls;
	unsigned int fax_calls;
	unsigned int fax_line;
	unsigned int fax_stream;
	/* fk.sends when the profile was taken: the upspeed re-INVITE must not
	 * be on the wire yet */
	unsigned int fax_sends;
	struct hal_fax_profile fax;
	enum hal_status fax_ret;
	/*
	 * The tone disabler release the return to voice asks for (call.h
	 * lec_latch_clear). It takes a line and no stream, so the recorder
	 * keeps the line beside the count; latch_ret is the refusal a case
	 * scripts, which must block nothing.
	 */
	unsigned int latch_clears;
	unsigned int latch_line;
	enum hal_status latch_ret;
	unsigned int last_line;
	unsigned int last_stream;
	unsigned int snapshot_line;
	unsigned int snapshot_stream;
	char last_digit;
	uint32_t last_duration;
	struct hal_media last;
	enum hal_status config_ret;
	/*
	 * The stop op answers for the first time here: the mode transition
	 * reads the status, where the teardown stop ignores it. stop_transient
	 * answers HAL_TRANSIENT that many times first, so a case can drive the
	 * retry-once contract of hal.h section 3 and the failure behind it
	 * separately.
	 */
	enum hal_status stop_ret;
	unsigned int stop_transient;
	/*
	 * The scripted entropy of a leg (call.h rand_bytes). rand[] is handed
	 * out in order, so a case knows every octet the leg keys with before it
	 * dials; past the script the op answers the filler of rand_fill_at(),
	 * so a case that scripts no key still knows it. rand_calls counts the
	 * op calls. rand_fail is the entropy failure the leg fails on, and
	 * rand_max fails every call past that count.
	 */
	uint8_t rand[128];
	size_t rand_len;
	size_t rand_next;
	unsigned int rand_calls;
	bool rand_fail;
	unsigned int rand_max;
	enum med_op seq[MED_SEQ_MAX];
	unsigned int nseq;
} med;

/* the filler octet the scripted op hands out for draw position n */
static uint8_t rand_fill_at(size_t n)
{
	return (uint8_t)(0xa0 + (n & 0x0f));
}

static void med_seq_add(enum med_op op)
{
	if (med.nseq < MED_SEQ_MAX)
		med.seq[med.nseq++] = op;
}

/* where the first op of this kind ran, -1 when it never did */
static int med_seq_index(enum med_op op)
{
	unsigned int i;

	for (i = 0; i < med.nseq; i++) {
		if (med.seq[i] == op)
			return (int)i;
	}

	return -1;
}

static bool bind_dev_is(const char *want)
{
	return !strcmp(med.last.bind_dev, want ? want : "");
}

static enum hal_status rec_config_set(void *priv, unsigned int line, unsigned int stream,
				      const struct hal_media *media)
{
	med.config_calls++;
	if (!media->tx_silence)
		med.config_live++;
	med.last_line = line;
	med.last_stream = stream;
	med.last = *media;
	med_seq_add(MED_CONFIG);

	return med.config_ret;
}

static enum hal_status rec_start(void *priv, unsigned int line, unsigned int stream)
{
	med.start_calls++;
	med_seq_add(MED_START);

	return HAL_OK;
}

static enum hal_status rec_stop(void *priv, unsigned int line, unsigned int stream)
{
	med.stop_calls++;
	med_seq_add(MED_STOP);

	if (med.stop_transient) {
		med.stop_transient--;
		return HAL_TRANSIENT;
	}

	return med.stop_ret;
}

static enum hal_status rec_dtmf(void *priv, unsigned int line, unsigned int stream, char digit,
				uint32_t duration_ms)
{
	med.dtmf_calls++;
	med.last_digit = digit;
	med.last_duration = duration_ms;
	med_seq_add(MED_DTMF);

	return HAL_OK;
}

static void rec_snapshot(void *priv, unsigned int line, unsigned int stream)
{
	med.snapshot_calls++;
	med.snapshot_line = line;
	med.snapshot_stream = stream;
	med_seq_add(MED_SNAPSHOT);
}

static enum hal_status rec_fax_apply(void *priv, unsigned int line, unsigned int stream,
				     const struct hal_fax_profile *fax)
{
	med.fax_calls++;
	med.fax_line = line;
	med.fax_stream = stream;
	med.fax_sends = fk.sends;
	med.fax = *fax;
	med_seq_add(MED_FAX);

	return med.fax_ret;
}

static enum hal_status rec_lec_latch_clear(void *priv, unsigned int line)
{
	med.latch_clears++;
	med.latch_line = line;

	return med.latch_ret;
}

static bool rec_rand_bytes(void *priv, void *buf, size_t len)
{
	uint8_t *out = buf;
	size_t i;

	med.rand_calls++;
	if (med.rand_fail)
		return false;
	if (med.rand_max && med.rand_calls > med.rand_max)
		return false;

	for (i = 0; i < len; i++, med.rand_next++) {
		out[i] = med.rand_next < med.rand_len ? med.rand[med.rand_next] :
			 rand_fill_at(med.rand_next);
	}

	return true;
}

static const struct call_media_ops rec_ops = {
	.config_set = rec_config_set,
	.start = rec_start,
	.stop = rec_stop,
	.dtmf_send = rec_dtmf,
	.stats_snapshot = rec_snapshot,
	.fax_apply = rec_fax_apply,
	.lec_latch_clear = rec_lec_latch_clear,
	.rand_bytes = rec_rand_bytes,
};

/* the same recorder with no entropy op bound, which is the plain-RTP fallback
 * of call.h rather than a refusal */
static const struct call_media_ops rec_ops_plain = {
	.config_set = rec_config_set,
	.start = rec_start,
	.stop = rec_stop,
	.dtmf_send = rec_dtmf,
	.stats_snapshot = rec_snapshot,
	.fax_apply = rec_fax_apply,
	.lec_latch_clear = rec_lec_latch_clear,
};

/* ----------------------------------------------------------- cb recorder */

static struct {
	unsigned int incoming, proceeding, answered, failed, remote_bye, remote_cancel;
	unsigned int hold_changed, reinvite_done, cw_net, ended, xfer_done;
	unsigned int started, started_line, started_slot, started_from, started_sends;
	bool started_inbound;
	/*
	 * The one outcome of a transfer (call.h cb.xfer_done). byes and sends
	 * are the wire counts at the moment it was reported, which is what pins
	 * the ordering call.h states: an accepted transfer has already BYEd its
	 * leg by the time the report fires.
	 */
	bool xfer_ok;
	uint32_t xfer_code;
	unsigned int xfer_line;
	unsigned int xfer_slot;
	unsigned int xfer_byes;
	unsigned int xfer_sends;
	unsigned int cw_sends;		/* fk.sends when the alert was reported */
	unsigned int cw_tone;
	char cw_name[CALL_CID_MAX];
	char cw_number[CALL_CID_MAX];
	bool cw_on;
	/* the out of band digit report (call.h cb.digit_net) */
	unsigned int digit_net;
	unsigned int digit_net_line;
	unsigned int digit_net_slot;
	unsigned int digit_net_sends;
	uint16_t digit_net_ms;
	char digit_net_digit;
	unsigned int slot;
	unsigned int line;
	unsigned int ring_id;
	char cid_number[CALL_CID_MAX];
	char cid_name[CALL_CID_MAX];
	bool cid_number_private;
	bool cid_name_private;
	bool early_media;
	bool held;
	bool reinvite_ok;
	enum call_fail_class cls;
	/* failed() hangs the slot up inside the report, as the line machine
	 * does for a connected call (fsm.c connected_failed()) */
	bool failed_hangs_up;
	/* the whole payload of the last ended() report, copied because the
	 * strings it carries live for the callback only */
	struct {
		unsigned int line;
		unsigned int slot;
		bool inbound;
		bool answered;
		bool failed;
		bool media_ran;
		bool cleared;
		uint32_t cleared_ago_ms;
		uint32_t answered_ago_ms;
		enum call_fail_class cls;
		uint8_t codec;
		bool t38;
		bool transferred;
		uint8_t ptime_ms;
		uint16_t rtp_port;
		char number[CALL_NUMBER_MAX];
		char remote_number[CALL_CID_MAX];
		char remote_name[CALL_CID_MAX];
		unsigned int stops;	/* med.stop_calls when it was reported */
		uint64_t at_ms;		/* fk.now when it was reported */
	} end;
	/* the report per (line, slot), because one teardown ends several legs
	 * and ev.end holds the last of them only */
	struct {
		unsigned int count;
		bool inbound;
		bool answered;
		bool cleared;
		bool transferred;
	} ends[CFG_CHANNEL_MAX][CALL_SLOTS];
} ev;

static void ev_incoming(unsigned int line, unsigned int slot, const char *cid_number,
			const char *cid_name, bool number_private, bool name_private,
			unsigned int ring_id)
{
	ev.incoming++;
	ev.line = line;
	ev.slot = slot;
	ev.ring_id = ring_id;
	ev.cid_number_private = number_private;
	ev.cid_name_private = name_private;
	snprintf(ev.cid_number, sizeof(ev.cid_number), "%s", cid_number ? cid_number : "");
	snprintf(ev.cid_name, sizeof(ev.cid_name), "%s", cid_name ? cid_name : "");
}

static void ev_proceeding(unsigned int line, unsigned int slot, bool early_media)
{
	ev.proceeding++;
	ev.line = line;
	ev.slot = slot;
	ev.early_media = early_media;
}

static void ev_answered(unsigned int line, unsigned int slot)
{
	ev.answered++;
	ev.line = line;
	ev.slot = slot;
}

static void ev_failed(unsigned int line, unsigned int slot, enum call_fail_class cls)
{
	ev.failed++;
	ev.line = line;
	ev.slot = slot;
	ev.cls = cls;
	if (ev.failed_hangs_up)
		call_hangup(line, slot);
}

static void ev_remote_bye(unsigned int line, unsigned int slot)
{
	ev.remote_bye++;
	ev.line = line;
	ev.slot = slot;
}

static void ev_remote_cancel(unsigned int line, unsigned int slot)
{
	ev.remote_cancel++;
	ev.line = line;
	ev.slot = slot;
}

static void ev_hold_changed(unsigned int line, unsigned int slot, bool held)
{
	ev.hold_changed++;
	ev.line = line;
	ev.held = held;
}

static void ev_reinvite_done(unsigned int line, unsigned int slot, bool ok)
{
	ev.reinvite_done++;
	ev.line = line;
	ev.reinvite_ok = ok;
}

static void ev_cw_net(unsigned int line, unsigned int slot, bool on, unsigned int tone,
		      const char *name, const char *number)
{
	ev.cw_net++;
	ev.line = line;
	ev.slot = slot;
	ev.cw_on = on;
	ev.cw_tone = tone;
	ev.cw_sends = fk.sends;
	snprintf(ev.cw_name, sizeof(ev.cw_name), "%s", name ? name : "");
	snprintf(ev.cw_number, sizeof(ev.cw_number), "%s", number ? number : "");
}

static void ev_digit_net(unsigned int line, unsigned int slot, char digit,
			 uint16_t duration_ms)
{
	ev.digit_net++;
	ev.digit_net_line = line;
	ev.digit_net_slot = slot;
	ev.digit_net_digit = digit;
	ev.digit_net_ms = duration_ms;
	ev.digit_net_sends = fk.sends;
}

/*
 * The record edge of a leg the layer made by itself (call.h cb.started). It
 * is the only announcement a transfer's child ever makes, so the record it
 * opens has no other oracle at all.
 */
static void ev_started(unsigned int line, unsigned int slot, bool inbound,
		       unsigned int from_slot)
{
	ev.started++;
	ev.started_line = line;
	ev.started_slot = slot;
	ev.started_from = from_slot;
	ev.started_inbound = inbound;
	ev.started_sends = fk.sends;
}

static void ev_ended(unsigned int line, unsigned int slot, const struct call_ended *info)
{
	ev.ended++;
	memset(&ev.end, 0, sizeof(ev.end));
	ev.end.line = line;
	ev.end.slot = slot;
	ev.end.inbound = info->inbound;
	ev.end.answered = info->answered;
	ev.end.failed = info->failed;
	ev.end.media_ran = info->media_ran;
	ev.end.cleared = info->cleared;
	ev.end.cleared_ago_ms = info->cleared_ago_ms;
	ev.end.answered_ago_ms = info->answered_ago_ms;
	ev.end.cls = info->fail_class;
	ev.end.codec = info->codec;
	ev.end.t38 = info->t38;
	ev.end.transferred = info->transferred;
	ev.end.ptime_ms = info->ptime_ms;
	ev.end.rtp_port = info->rtp_port;
	ev.end.stops = med.stop_calls;
	ev.end.at_ms = fk.now;
	snprintf(ev.end.number, sizeof(ev.end.number), "%s",
		 info->number ? info->number : "");
	snprintf(ev.end.remote_number, sizeof(ev.end.remote_number), "%s",
		 info->remote_number ? info->remote_number : "");
	snprintf(ev.end.remote_name, sizeof(ev.end.remote_name), "%s",
		 info->remote_name ? info->remote_name : "");

	if (line >= CFG_CHANNEL_MAX || slot >= CALL_SLOTS)
		return;

	ev.ends[line][slot].count++;
	ev.ends[line][slot].inbound = info->inbound;
	ev.ends[line][slot].answered = info->answered;
	ev.ends[line][slot].cleared = info->cleared;
	ev.ends[line][slot].transferred = info->transferred;
}

/* the wire counter of the message section below: the transfer outcome is
 * recorded against what was already sent when it fired */
static unsigned int count_send(const char *what);

static void ev_xfer_done(unsigned int line, unsigned int slot, bool ok, uint32_t code)
{
	ev.xfer_done++;
	ev.xfer_line = line;
	ev.xfer_slot = slot;
	ev.xfer_ok = ok;
	ev.xfer_code = code;
	ev.xfer_byes = count_send("BYE");
	ev.xfer_sends = fk.sends;
}

static const struct call_cb rec_cb = {
	.incoming = ev_incoming,
	.proceeding = ev_proceeding,
	.answered = ev_answered,
	.failed = ev_failed,
	.remote_bye = ev_remote_bye,
	.remote_cancel = ev_remote_cancel,
	.hold_changed = ev_hold_changed,
	.reinvite_done = ev_reinvite_done,
	.xfer_done = ev_xfer_done,
	.cw_net = ev_cw_net,
	.started = ev_started,
	.ended = ev_ended,
	.digit_net = ev_digit_net,
};

/* ------------------------------------------------------------------ config */

static void cfg_default(void)
{
	struct cfg_channel *ch;
	unsigned int i;

	memset(&cfg, 0, sizeof(cfg));
	cfg.loaded = true;
	cfg.enabled = true;
	cfg.channels = CFG_CHANNEL_MAX;
	cfg.rtcp_enabled = true;
	cfg.rtcp_interval = 5;
	snprintf(cfg.local_interface, sizeof(cfg.local_interface), "%s", "wan");
	snprintf(cfg.alert_info_prefix, sizeof(cfg.alert_info_prefix), "%s", "Bellcore-dr");

	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		ch = &cfg.channel[i];
		ch->present = true;
		ch->enabled = true;
		ch->id = i;
		ch->dtmf_mode = CFG_DTMF_RFC2833;
		/* the schema default is 1, and the fixture zeroes the struct, so
		 * a case that never names the option runs the shipped rule.
		 * early_media_enabled has the same schema default and is left
		 * at 0 here on purpose: a case that names no early media then
		 * runs with no early stream, and the eight that exercise the
		 * option set it themselves */
		ch->fax_upspeed_follow = true;
		/* same rule for the URI escape policy of digit_esc_enabled */
		ch->digit_esc = 1;
		ch->ptime_ms = 20;
		ch->g711a_ptype = 8;
		ch->g729a_ptype = 18;
		ch->jitter_target = 50;
		/* the default ceiling of the adaptive jitter buffer is 250 ms */
		ch->jitter_max = 250;
		ch->rtp_tos = 184;
		ch->rtcp_tos = 184;
		ch->rtp_port_min = 10000;
		ch->rtp_port_max = 10030;
		ch->session_refresher = CFG_REFRESH_UNSPEC;
	}
}

/* --------------------------------------------------------------- messages */

static char text_buf[8][TEXT_MAX];
static unsigned int text_next;

static char *text_slot(void)
{
	return text_buf[text_next++ % 8];
}

/*
 * The block a registrar's Service-Route becomes, built by the production
 * collector out of a real 200 OK. A hand-filled block could hold hops
 * route_hdrs_take() refuses, and the leg would then carry a route the daemon
 * can never produce.
 */
static const struct sip_route_hdrs *sr_take(const char *hdrs)
{
	static struct sip_route_hdrs sr;
	char *buf = text_slot();
	struct sip_msg *m;

	snprintf(buf, TEXT_MAX, "SIP/2.0 200 OK\r\n%sContent-Length: 0\r\n\r\n",
		 hdrs);
	m = msg_take(buf);
	route_hdrs_clear(&sr);
	if (m)
		route_hdrs_take(&sr, m, SIP_HDR_SERVICE_ROUTE);

	return &sr;
}

struct req_info {
	char method[16];
	char branch[80];
	char call_id[160];
	char from[240];
	char to[240];
	char from_tag[64];
	char to_tag[64];
	char contact[240];
	char ruri[240];
	char body[1200];
	uint32_t cseq;
	size_t body_len;
	bool ok;
};

/*
 * What one entry of the status dump says about a leg. It stands here, in front
 * of its reader in the status dump section, because the re-INVITE suites run
 * before that section and read the state word back.
 */
struct slot_view {
	char codec[16];
	char remote[CALL_CID_MAX];
	char name[CALL_CID_MAX];
	/* the fax state of this call, empty when the key is absent */
	char fax[16];
	/* what the passthrough carries, empty when the key is absent */
	char fax_kind[16];
	/* the SDES suite of this call, empty when the key is absent */
	char srtp[32];
	char state[24];			/* call.c state_name() */
	unsigned int ptime;
	bool found;
	bool has_ptime;
	bool cid_private;
	/* the UI occupancy of the leg, which is what admission and leg_alloc
	 * read: a draining leg that still holds it refuses a new call */
	bool ui_busy;
	/* the two names of the leg (call.h): ui_slot is always written, and
	 * shadow only for a leg that holds no name at all */
	unsigned int ui_slot;
	bool has_ui_slot;
	bool shadow;
	bool has_shadow;
};

static bool slot_view_get(unsigned int line, unsigned int slot, struct slot_view *v);

static bool req_info_at(unsigned int idx, struct req_info *ri)
{
	struct sip_msg m;
	struct sip_addr a;
	struct sip_via via;
	struct sip_str s;

	memset(ri, 0, sizeof(*ri));
	if (!send_parse(idx, &m) || !m.is_request)
		return false;

	sip_str_copy(ri->method, sizeof(ri->method), m.method_name);
	sip_str_copy(ri->ruri, sizeof(ri->ruri), m.ruri);
	if (hdr_via_top(&m, &via))
		sip_str_copy(ri->branch, sizeof(ri->branch), via.branch);
	if (hdr_call_id_get(&m, &s))
		sip_str_copy(ri->call_id, sizeof(ri->call_id), s);
	if (msg_hdr_value(&m, SIP_HDR_FROM, &s))
		sip_str_copy(ri->from, sizeof(ri->from), s);
	if (msg_hdr_value(&m, SIP_HDR_TO, &s))
		sip_str_copy(ri->to, sizeof(ri->to), s);
	if (hdr_addr_get(&m, SIP_HDR_FROM, &a))
		sip_str_copy(ri->from_tag, sizeof(ri->from_tag), a.tag);
	if (hdr_addr_get(&m, SIP_HDR_TO, &a))
		sip_str_copy(ri->to_tag, sizeof(ri->to_tag), a.tag);
	if (hdr_addr_get(&m, SIP_HDR_CONTACT, &a))
		sip_str_copy(ri->contact, sizeof(ri->contact), a.uri.raw);
	hdr_cseq_get(&m, &ri->cseq, NULL);
	if (msg_body_get(&m, &s) && s.len < sizeof(ri->body)) {
		memcpy(ri->body, s.p, s.len);
		ri->body[s.len] = 0;
		ri->body_len = s.len;
	}

	ri->ok = true;

	return true;
}

/* the index of the last message whose start line begins with what */
static int find_send(const char *what)
{
	unsigned int i, n = fk.sends < FAKE_SEND_MAX ? fk.sends : FAKE_SEND_MAX;
	int found = -1;

	for (i = 0; i < n; i++) {
		if (!strncmp(fk.send[i].buf, what, strlen(what)))
			found = (int)i;
	}

	return found;
}

static unsigned int count_send(const char *what)
{
	unsigned int i, n = fk.sends < FAKE_SEND_MAX ? fk.sends : FAKE_SEND_MAX, c = 0;

	for (i = 0; i < n; i++) {
		if (!strncmp(fk.send[i].buf, what, strlen(what)))
			c++;
	}

	return c;
}

static bool has(const char *hay, const char *needle)
{
	return hay && strstr(hay, needle) != NULL;
}

/* how many times a header name opens a line of one message: the "exactly one
 * Replaces" rule of RFC 3891 has no other oracle */
static unsigned int count_hdr(const char *msg, const char *name)
{
	size_t n = strlen(name);
	unsigned int c = 0;
	const char *p;

	for (p = msg; p && *p; p++) {
		if (p != msg && (p[-1] != '\n'))
			continue;
		if (!strncmp(p, name, n))
			c++;
	}

	return c;
}

static bool last_req(struct req_info *ri)
{
	return fk.sends ? req_info_at(fk.sends - 1, ri) : false;
}

static bool req_of(const char *method, struct req_info *ri)
{
	int idx = find_send(method);

	return idx >= 0 && req_info_at((unsigned int)idx, ri);
}

/* an SDP body with one or more payload types */
static const char *sdp_body(const char *addr, unsigned int port, const char *pts,
			    const char *extra)
{
	char *buf = text_slot();

	snprintf(buf, TEXT_MAX,
		 "v=0\r\n"
		 "o=- 42 42 IN IP4 %s\r\n"
		 "s=-\r\n"
		 "c=IN IP4 %s\r\n"
		 "t=0 0\r\n"
		 "m=audio %u RTP/AVP %s\r\n"
		 "a=rtpmap:0 PCMU/8000\r\n"
		 "a=rtpmap:8 PCMA/8000\r\n"
		 "a=rtpmap:101 telephone-event/8000\r\n"
		 "%s",
		 addr, addr, port, pts, extra ? extra : "");

	return buf;
}

/* the same body in the IPv6 family: RFC 4566 names it IP6 and the address is
 * written bare, without the brackets a URI puts around it */
static const char *sdp_body6(const char *addr, unsigned int port,
			     const char *pts, const char *extra)
{
	char *buf = text_slot();

	snprintf(buf, TEXT_MAX,
		 "v=0\r\n"
		 "o=- 42 42 IN IP6 %s\r\n"
		 "s=-\r\n"
		 "c=IN IP6 %s\r\n"
		 "t=0 0\r\n"
		 "m=audio %u RTP/AVP %s\r\n"
		 "a=rtpmap:0 PCMU/8000\r\n"
		 "a=rtpmap:8 PCMA/8000\r\n"
		 "a=rtpmap:101 telephone-event/8000\r\n"
		 "%s",
		 addr, addr, port, pts, extra ? extra : "");

	return buf;
}

static const char *resp_text(const struct req_info *ri, uint32_t code, const char *to_tag,
			     const char *extra, const char *body)
{
	char *buf = text_slot();
	char to[300];

	if (to_tag && !ri->to_tag[0])
		snprintf(to, sizeof(to), "%s;tag=%s", ri->to, to_tag);
	else
		snprintf(to, sizeof(to), "%s", ri->to);

	snprintf(buf, TEXT_MAX,
		 "SIP/2.0 %u %s\r\n"
		 "Via: SIP/2.0/UDP %s:%u;branch=%s\r\n"
		 "From: %s\r\n"
		 "To: %s\r\n"
		 "Call-ID: %s\r\n"
		 "CSeq: %u %s\r\n"
		 "%s"
		 "%s"
		 "%s"
		 "Content-Length: %u\r\n"
		 "\r\n"
		 "%s",
		 code, sip_status_reason(code), T_HOST, T_PORT, ri->branch, ri->from, to,
		 ri->call_id, ri->cseq, ri->method,
		 (fk.no_contact || has(extra, "Contact:")) ? "" : T_CONTACT,
		 extra ? extra : "",
		 body ? "Content-Type: application/sdp\r\n" : "",
		 (unsigned int)(body ? strlen(body) : 0), body ? body : "");

	return buf;
}

/* ------------------------------------------------------------- the router */

/* the fixture's Call-ID index, standing in for the per account dialog sets */
static void route_remember_id(const char *call_id, unsigned int line)
{
	unsigned int i;

	for (i = 0; i < 4; i++) {
		if (fk.route[i].used)
			continue;
		snprintf(fk.route[i].call_id, sizeof(fk.route[i].call_id), "%s", call_id);
		fk.route[i].line = line;
		fk.route[i].used = true;
		fk.routes++;
		return;
	}
}

static void route_remember(const struct sip_msg *msg, unsigned int line)
{
	char cid[160];
	struct sip_str s;

	if (!hdr_call_id_get(msg, &s) || !sip_str_copy(cid, sizeof(cid), s))
		return;

	route_remember_id(cid, line);
}

/*
 * Mirrors account_for_request(): with one socket for both lines the socket
 * cannot name the line, so an in-dialog request follows its dialog, a CANCEL
 * follows the INVITE it targets, a new call follows the user in the request
 * URI, and only what none of that names falls back to the socket owner.
 */
static unsigned int route_line(const struct sip_msg *msg)
{
	struct sip_str cid;
	struct sip_uri uri;
	unsigned int i;

	if (hdr_call_id_get(msg, &cid)) {
		for (i = 0; i < 4; i++) {
			if (fk.route[i].used && sip_str_eq(cid, fk.route[i].call_id))
				return fk.route[i].line;
		}
	}

	if (msg->method == SIP_METHOD_CANCEL) {
		uint32_t target = sip_txn_cancel_target(&mgr, msg);

		for (i = 0; i < CFG_CHANNEL_MAX; i++) {
			if (call_txn_local_tag(i, target))
				return i;
		}
	}

	if (uri_parse(msg->ruri, &uri) && uri.user.len) {
		for (i = 0; i < CFG_CHANNEL_MAX; i++) {
			if (sip_str_eq(uri.user, line_user(i)))
				return i;
		}
	}

	return fk.line;
}

/*
 * The account layer's NOTIFY intake (account.c account_notify_recv), mirrored
 * so a NOTIFY reaches the call layer through the same route it travels in the
 * daemon and no case pins a path the daemon does not have. The intake answers
 * the packages the account serves and declines the one a call leg owns, and
 * account.c's request intake skips this intake outright for a refer NOTIFY
 * that matches a dialog.
 */
static bool notify_intake(struct sip_msg *msg, struct sip_txn *txn)
{
	struct ua_resp r;

	memset(&r, 0, sizeof(r));

	switch (sip_sub_event_class(msg)) {
	case SIP_SUB_EV_MWI:
		fk.mwi_notifies++;
		r.code = 200;
		break;
	case SIP_SUB_EV_OTHER:
		r.code = 489;
		break;
	case SIP_SUB_EV_REFER:
	case SIP_SUB_EV_NONE:
		return false;
	}

	ua_response_send(&ua, txn, msg, &r);

	return true;
}

static void request_recv(struct sip_msg *msg, const struct sip_txn_dst *src)
{
	unsigned int slot = 0;
	unsigned int line;
	struct sip_txn *txn;
	struct sip_dialog *d;
	uint32_t owner, target;
	bool new_invite;

	switch (ua_msg_recv(&ua, msg)) {
	case UA_RX_ABSORBED:
		return;
	case UA_RX_ACK:
		call_rx_ack(route_line(msg), msg);
		return;
	case UA_RX_PASS:
		break;
	}

	if (msg->method == SIP_METHOD_ACK)
		return;

	line = route_line(msg);

	if (msg->method == SIP_METHOD_CANCEL) {
		target = sip_txn_cancel_target(&mgr, msg);
		txn = sip_txn_server_start(&mgr, msg, src,
					   acct_owner_make(ACCT_OWNER_UA, line + 1));
		if (!txn)
			return;
		if (ua_cancel_handle(&ua, msg, txn, call_txn_local_tag(line, target)))
			call_rx_cancel(line, target);
		return;
	}

	d = sip_dialog_find(&dlgs, msg);
	new_invite = msg->method == SIP_METHOD_INVITE && !d;

	owner = acct_owner_make(ACCT_OWNER_UA, line + 1);
	if (new_invite && call_slot_free(line, &slot))
		owner = acct_owner_make(ACCT_OWNER_CALL, call_slot_handle(line, slot));

	txn = sip_txn_server_start(&mgr, msg, src, owner);
	if (!txn)
		return;

	if (msg->method == SIP_METHOD_INVITE)
		ua_trying_arm(&ua, txn);

	if (ua_request_policy(&ua, msg, txn) != UA_POLICY_PASS)
		return;

	if (!(d && sip_sub_event_class(msg) == SIP_SUB_EV_REFER) &&
	    notify_intake(msg, txn))
		return;

	if (new_invite) {
		struct ua_resp r;

		memset(&r, 0, sizeof(r));

		/* the account is unregistering: not busy, going away */
		if (!fk.active)
			r.code = 480;
		/* no live registration: no new call, apart from a Replaces,
		 * and in front of the busy answer (account.c) */
		else if (!fk.registered && !hdr_val_count(msg, SIP_HDR_REPLACES))
			r.code = 480;
		else if (!call_slot_free(line, &slot))
			r.code = 486;

		if (r.code) {
			ua_response_send(&ua, txn, msg, &r);
			return;
		}

		route_remember(msg, line);
	}

	call_rx_request(line, txn, msg, src);
}

static void rx_msg(struct sip_msg *msg)
{
	struct sip_txn *txn = NULL;
	enum sip_txn_rx rx = sip_txn_msg_recv(&mgr, msg, &txn);

	switch (rx) {
	case SIP_RX_ABSORBED:
	case SIP_RX_DELIVERED:
	case SIP_RX_IGNORED:
		return;
	case SIP_RX_REQUEST_NEW:
	case SIP_RX_ACK_2XX:
		request_recv(msg, &fk.peer);
		return;
	case SIP_RX_RESPONSE_STRAY:
		if (sip_dialog_resp_recv(&dlgs, msg) == SIP_DLG_RX_ABSORBED)
			return;
		call_rx_stray(fk.line, msg);
		return;
	}
}

static void rx(const char *text)
{
	struct sip_msg *m = msg_take(text);

	if (m)
		rx_msg(m);
}

/*
 * An INVITE handed straight to the call layer, with the owner account.c would
 * have given it. account.c's request intake answers a full line 486 and an
 * unregistering account 480 on its own, so the cases that pin the admission
 * order inside call.c drive the request this way. The answer is the handle of
 * the server transaction, which is how a case ends that transaction under the
 * leg it started.
 */
static uint32_t rx_direct(unsigned int line, const char *text)
{
	struct sip_msg *m = msg_take(text);
	unsigned int slot = 0;
	struct sip_txn *txn;
	uint32_t owner, handle;

	CHECK(m != NULL);
	if (!m)
		return SIP_HANDLE_NONE;

	owner = call_slot_free(line, &slot) ?
		acct_owner_make(ACCT_OWNER_CALL, call_slot_handle(line, slot)) :
		acct_owner_make(ACCT_OWNER_UA, line + 1);

	txn = sip_txn_server_start(&mgr, m, &fk.peer, owner);
	CHECK(txn != NULL);
	if (!txn)
		return SIP_HANDLE_NONE;

	handle = sip_txn_handle(txn);
	call_rx_request(line, txn, m, &fk.peer);

	return handle;
}

/*
 * An in-dialog request handed straight to the call layer, with the owner
 * account.c gives one. The NOTIFY intake of account.c answers some packages
 * by itself, so a case that pins what call.c does with one of those has to
 * bypass it, exactly as rx_direct() bypasses the admission answers.
 */
static void rx_in_dialog_direct(unsigned int line, const char *text)
{
	struct sip_msg *m = msg_take(text);
	struct sip_txn *txn;

	CHECK(m != NULL);
	if (!m)
		return;

	txn = sip_txn_server_start(&mgr, m, &fk.peer,
				   acct_owner_make(ACCT_OWNER_UA, line + 1));
	CHECK(txn != NULL);
	if (txn)
		call_rx_request(line, txn, m, &fk.peer);
}

/* answer the last request that went out */
static void reply(uint32_t code, const char *to_tag, const char *extra, const char *body)
{
	struct req_info ri;

	if (!last_req(&ri))
		return;

	rx(resp_text(&ri, code, to_tag, extra, body));
}

/* answer a specific earlier request, for a race the fixture builds by hand */
static void reply_at(unsigned int idx, uint32_t code, const char *to_tag, const char *extra,
		     const char *body)
{
	struct req_info ri;

	if (!req_info_at(idx, &ri))
		return;

	rx(resp_text(&ri, code, to_tag, extra, body));
}

/* --------------------------------------------------------------- fixture */

static void fx_reset(void)
{
	struct sip_timer_cfg tc;

	calls_done();
	ua_done(&ua);
	sip_dialog_set_done(&dlgs);
	sip_txn_mgr_done(&mgr);

	memset(&fk, 0, sizeof(fk));
	memset(&med, 0, sizeof(med));
	memset(&ev, 0, sizeof(ev));
	memset(&tr, 0, sizeof(tr));
	fk.now = 100000;
	fk.env.send = fake_send_fn;
	fake_clock_bind(&fk.env);
	fk.env.base.trace = fake_trace;
	fk.env.resp = fake_resp;
	fk.env.done = fake_done;
	fk.view_ok = true;
	fk.dst_valid = true;
	fk.registered = true;
	fk.active = true;
	fk.local_host = T_HOST;
	fk.domain = T_DOMAIN;
	fk.hal_state = UVOICE_HAL_READY;
	sip_dst_set(&fk.peer, T_PEER, T_PEER_PORT);

	sip_timer_cfg_default(&tc);
	sip_txn_mgr_init(&mgr, &fk.env, &tc);
	ua_init(&ua, &mgr, &fk.env, NULL, &ua_cb, NULL);
	sip_dialog_set_init(&dlgs, &fk.env);

	calls_init(&rec_ops, &rec_cb);
	calls_reload(&cfg);
	/* after calls_init(), which clears them, the way sip_init() binds them */
	calls_host_bind(proto_host_fake());
	calls_hal_ready_bind(fake_hal_ready);
	calls_line_admit_bind(fake_line_admit);
	/* the default fixture backend carries no optional feature, so a leg is
	 * T.38 capable only where a case says so (t38_enable) */
	calls_hal_features_set(0);
	/* and it carries every codec, so a case that is not about the mask
	 * offers what the config names (call.h calls_hal_codecs_set) */
	calls_hal_codecs_set(~0u);
	net_port_reset();
}

/* an inbound INVITE, with an optional extra header block and body */
static const char *invite_text_to(const char *user, const char *call_id, const char *extra,
				  const char *body)
{
	char *buf = text_slot();

	snprintf(buf, TEXT_MAX,
		 "INVITE sip:%s@%s SIP/2.0\r\n"
		 "Via: SIP/2.0/UDP %s:%u;branch=z9hG4bK-in-%s\r\n"
		 "Max-Forwards: 70\r\n"
		 "From: \"Alice\" <sip:1002@%s>;tag=peer-%s\r\n"
		 "To: <sip:%s@%s>\r\n"
		 "Call-ID: %s\r\n"
		 "CSeq: 1 INVITE\r\n"
		 "Contact: <sip:1002@%s:%u>\r\n"
		 "Record-Route: <sip:proxy.example.net;lr>\r\n"
		 "Supported: timer\r\n"
		 "%s"
		 "%s"
		 "Content-Length: %u\r\n"
		 "\r\n"
		 "%s",
		 user, T_DOMAIN, T_PEER, T_PEER_PORT, call_id, T_DOMAIN, call_id, user,
		 T_DOMAIN, call_id, T_PEER, T_PEER_PORT, extra ? extra : "",
		 body ? "Content-Type: application/sdp\r\n" : "",
		 (unsigned int)(body ? strlen(body) : 0), body ? body : "");

	return buf;
}

static const char *invite_text(const char *call_id, const char *extra, const char *body)
{
	return invite_text_to(T_USER, call_id, extra, body);
}

/* a built message with one whole header line taken out, for a peer that
 * omits a header every builder writes */
static const char *text_line_drop(const char *text, const char *line)
{
	const char *at = strstr(text, line);
	char *buf;

	if (!at)
		return text;

	buf = text_slot();
	snprintf(buf, TEXT_MAX, "%.*s%s", (int)(at - text), text, at + strlen(line));

	return buf;
}

/*
 * An inbound INVITE whose From this case writes itself, which is the only way
 * to drive the caller identity paths: everything else matches invite_text().
 */
static const char *invite_text_from(const char *call_id, const char *from,
				    const char *extra, const char *body)
{
	char *buf = text_slot();

	snprintf(buf, TEXT_MAX,
		 "INVITE sip:%s@%s SIP/2.0\r\n"
		 "Via: SIP/2.0/UDP %s:%u;branch=z9hG4bK-in-%s\r\n"
		 "Max-Forwards: 70\r\n"
		 "From: %s;tag=peer-%s\r\n"
		 "To: <sip:%s@%s>\r\n"
		 "Call-ID: %s\r\n"
		 "CSeq: 1 INVITE\r\n"
		 "Contact: <sip:1002@%s:%u>\r\n"
		 "Supported: timer\r\n"
		 "%s"
		 "%s"
		 "Content-Length: %u\r\n"
		 "\r\n"
		 "%s",
		 T_USER, T_DOMAIN, T_PEER, T_PEER_PORT, call_id, from, call_id, T_USER,
		 T_DOMAIN, call_id, T_PEER, T_PEER_PORT, extra ? extra : "",
		 body ? "Content-Type: application/sdp\r\n" : "",
		 (unsigned int)(body ? strlen(body) : 0), body ? body : "");

	return buf;
}

/*
 * An inbound INVITE whose From carries no tag. RFC 3261 8.1.1.3 requires one,
 * so every other builder here writes one, and this side can build no dialog
 * without it.
 */
static const char *invite_text_untagged(const char *call_id, const char *body)
{
	char *buf = text_slot();

	snprintf(buf, TEXT_MAX,
		 "INVITE sip:%s@%s SIP/2.0\r\n"
		 "Via: SIP/2.0/UDP %s:%u;branch=z9hG4bK-in-%s\r\n"
		 "Max-Forwards: 70\r\n"
		 "From: \"Alice\" <sip:1002@%s>\r\n"
		 "To: <sip:%s@%s>\r\n"
		 "Call-ID: %s\r\n"
		 "CSeq: 1 INVITE\r\n"
		 "Contact: <sip:1002@%s:%u>\r\n"
		 "Supported: timer\r\n"
		 "%s"
		 "Content-Length: %u\r\n"
		 "\r\n"
		 "%s",
		 T_USER, T_DOMAIN, T_PEER, T_PEER_PORT, call_id, T_DOMAIN, T_USER,
		 T_DOMAIN, call_id, T_PEER, T_PEER_PORT,
		 body ? "Content-Type: application/sdp\r\n" : "",
		 (unsigned int)(body ? strlen(body) : 0), body ? body : "");

	return buf;
}

/* the offer of a field capture, byte for byte */
static const char field_sdp[] =
	"v=0\r\n"
	"o=- 4086519302 1767225600 IN IP4 203.0.113.88\r\n"
	"s=-\r\n"
	"e=unknown@invalid.net\r\n"
	"c=IN IP4 203.0.113.88\r\n"
	"t=0 0\r\n"
	"m=audio 23634 RTP/AVP 0 101 18\r\n"
	"a=silenceSupp:off - - - -\r\n"
	"a=sqn: 0\r\n"
	"a=cdsc:1 image udptl t38\r\n"
	"a=rtpmap:0 PCMU/8000\r\n"
	"a=rtpmap:101 telephone-event/8000\r\n"
	"a=fmtp:101 0-15\r\n"
	"a=rtpmap:18 G729/8000\r\n"
	"a=sendrecv\r\n"
	"a=ptime:20\r\n";

/*
 * The inbound INVITE of the field capture, with the header set
 * and the header order of the capture: a request URI whose user is a token,
 * a To with a header parameter and no angle brackets, a Contact with no user
 * part, two Supported headers, a Session-ID and the field offer. The display
 * name repeats the user part, verstat parameter and all. In the field the
 * token is the SIP user name of the line, and account.c finds the line by
 * that name, above this suite; here no line carries it, so the request lands
 * on the owner of the socket. The branch and the Call-ID follow the fixture,
 * so two calls are two transactions.
 */
static const char *invite_text_field(const char *call_id)
{
	char *buf = text_slot();

	snprintf(buf, TEXT_MAX,
		 "INVITE sip:oRYOOqsGFzQpUt2dOjeCRZ0@192.168.210.10:5065"
		 ";transport=UDP SIP/2.0\r\n"
		 "Via: SIP/2.0/UDP 198.51.100.201:5065;branch=z9hG4bK-in-%s\r\n"
		 "Contact: <sip:198.51.100.201:5065;transport=udp>\r\n"
		 "CSeq: 22979 INVITE\r\n"
		 "Content-Length: %u\r\n"
		 "Content-Type: application/sdp\r\n"
		 "Call-ID: %s\r\n"
		 "From: \"2015550123;verstat=No-TN-Validation\""
		 " <sip:2015550123;verstat=No-TN-Validation"
		 "@198.51.100.201:5065>"
		 ";tag=IJvv2uh23-L77J20260101000000N3706433\r\n"
		 "To: sip:oRYOOqsGFzQpUt2dOjeCRZ0@203.0.113.251:10311"
		 ";transport=udp\r\n"
		 "Session-ID: 8dcc4b8f4aa366485628a232f8e358114d18792f\r\n"
		 "Max-Forwards: 45\r\n"
		 "Session-Expires: 1800;refresher=uac\r\n"
		 "Supported: 100rel\r\n"
		 "Supported: timer\r\n"
		 "Allow: INVITE, ACK, PRACK, CANCEL, BYE, REFER, NOTIFY,"
		 " OPTIONS, MESSAGE, INFO, REGISTER, SUBSCRIBE\r\n"
		 "\r\n"
		 "%s",
		 call_id, (unsigned int)sizeof(field_sdp) - 1, call_id,
		 field_sdp);

	return buf;
}

/*
 * call_dial plus the fixture's note of which line owns the Call-ID. The
 * INVITE is found by method and not taken as the first send: a dial that
 * evicts a draining leg puts that leg's BYE on the wire first.
 */
static bool dial_present(unsigned int line, unsigned int slot, const char *number,
			 bool cid_present)
{
	unsigned int before = fk.sends, i;
	struct req_info ri;

	if (!call_dial(line, slot, number, cid_present))
		return false;

	for (i = before; i < fk.sends; i++) {
		if (req_info_at(i, &ri) && !strcmp(ri.method, "INVITE")) {
			route_remember_id(ri.call_id, line);
			break;
		}
	}

	return true;
}

static bool dial(unsigned int line, unsigned int slot, const char *number)
{
	return dial_present(line, slot, number, true);
}

/*
 * A request from the peer inside an established dialog. The two directions
 * differ only in where the tags come from: as a UAS ours is the To tag of the
 * 2xx we sent, as a UAC ours is the From tag of the INVITE we sent.
 */
static const char *dialog_req_text(const char *method, unsigned int cseq, const char *call_id,
				   const char *peer_tag, const char *our_tag, const char *extra,
				   const char *body)
{
	char *buf = text_slot();

	snprintf(buf, TEXT_MAX,
		 "%s sip:%s@%s:%u SIP/2.0\r\n"
		 "Via: SIP/2.0/UDP %s:%u;branch=z9hG4bK-ind-%u-%s\r\n"
		 "Max-Forwards: 70\r\n"
		 "From: <sip:1002@%s>;tag=%s\r\n"
		 "To: <sip:%s@%s>;tag=%s\r\n"
		 "Call-ID: %s\r\n"
		 "CSeq: %u %s\r\n"
		 "%s"
		 "%s"
		 "%s"
		 "Content-Length: %u\r\n"
		 "\r\n"
		 "%s",
		 method, T_USER, T_HOST, T_PORT, T_PEER, T_PEER_PORT, cseq, method, T_DOMAIN,
		 peer_tag, T_USER, T_DOMAIN, our_tag, call_id, cseq, method,
		 has(extra, "Contact:") ? "" : T_CONTACT, extra ? extra : "",
		 body && !has(extra, "Content-Type:") ? "Content-Type: application/sdp\r\n" : "",
		 (unsigned int)(body ? strlen(body) : 0), body ? body : "");

	return buf;
}

/*
 * The three dialog ids of a 2xx this side sent, named by its send index. A
 * fixture that answers two INVITEs on one line cannot ask for "the last 200":
 * the answer to the second call and the answer to a BYE of the first are both
 * later than the one it means.
 */
static bool resp_tags_at(int idx, char *call_id, size_t clen, char *peer_tag, size_t plen,
			 char *our_tag, size_t olen)
{
	static char copy[WIRE_MAX];
	struct sip_addr from, to;
	struct sip_str cid;
	struct sip_msg m;
	size_t len;

	if (idx < 0 || (unsigned int)idx >= fk.sends || idx >= FAKE_SEND_MAX)
		return false;

	len = fk.send[idx].len;
	memcpy(copy, fk.send[idx].buf, len);
	if (msg_parse(&m, copy, len) != SIP_PARSE_OK)
		return false;
	if (!hdr_call_id_get(&m, &cid) || !hdr_addr_get(&m, SIP_HDR_FROM, &from) ||
	    !hdr_addr_get(&m, SIP_HDR_TO, &to))
		return false;

	sip_str_copy(call_id, clen, cid);
	sip_str_copy(peer_tag, plen, from.tag);
	sip_str_copy(our_tag, olen, to.tag);

	return true;
}

/* our own last 2xx names both tags of an inbound call */
static bool uas_tags(char *call_id, size_t clen, char *peer_tag, size_t plen, char *our_tag,
		     size_t olen)
{
	return resp_tags_at(find_send("SIP/2.0 200"), call_id, clen, peer_tag, plen, our_tag,
			    olen);
}

static const char *uas_in_dialog(const char *method, unsigned int cseq, const char *extra,
				 const char *body)
{
	char call_id[160], peer_tag[64], our_tag[64];

	if (!uas_tags(call_id, sizeof(call_id), peer_tag, sizeof(peer_tag), our_tag,
		      sizeof(our_tag)))
		return "";

	return dialog_req_text(method, cseq, call_id, peer_tag, our_tag, extra, body);
}

static const char *uac_in_dialog(const char *method, const char *peer_tag, unsigned int cseq,
				 const char *extra, const char *body)
{
	struct req_info ri;

	if (!req_of("INVITE", &ri))
		return "";

	return dialog_req_text(method, cseq, ri.call_id, peer_tag, ri.from_tag, extra, body);
}
/* ------------------------------------------------------------ port pool */

static void test_ports(void)
{
	uint16_t p = 0, q = 0, r = 0;

	CASE("port: first allocation takes the range base");
	net_port_reset();
	CHECK(net_port_alloc(10000, 10030, &p));
	CHECK_UINT(p, 10000);

	CASE("port: the second is the next even one");
	CHECK(net_port_alloc(10000, 10030, &q));
	CHECK_UINT(q, 10002);

	CASE("port: a freed port is not handed out again at once");
	net_port_free(p);
	CHECK(net_port_alloc(10000, 10030, &r));
	CHECK_UINT(r, 10004);

	CASE("port: the cursor wraps onto the freed port");
	net_port_free(q);
	net_port_free(r);
	net_port_reset();
	CHECK(net_port_alloc(10000, 10003, &p));
	CHECK(net_port_alloc(10000, 10003, &q));
	CHECK_UINT(p, 10000);
	CHECK_UINT(q, 10002);
	net_port_free(p);
	CHECK(net_port_alloc(10000, 10003, &r));
	CHECK_UINT(r, 10000);

	CASE("port: an exhausted range refuses");
	net_port_reset();
	CHECK(net_port_alloc(10000, 10001, &p));
	CHECK_UINT(p, 10000);
	CHECK(!net_port_alloc(10000, 10001, &q));

	CASE("port: an odd lower bound rounds up to the next even port");
	net_port_reset();
	CHECK(net_port_alloc(10001, 10005, &p));
	CHECK_UINT(p, 10002);

	CASE("port: an odd upper bound rounds down");
	net_port_reset();
	CHECK(net_port_alloc(10002, 10003, &p));
	CHECK_UINT(p, 10002);
	CHECK(!net_port_alloc(10002, 10003, &q));

	CASE("port: an unset range takes the default 10000..10030");
	net_port_reset();
	CHECK(net_port_alloc(0, 0, &p));
	CHECK_UINT(p, 10000);

	CASE("port: an inverted range takes the default too");
	net_port_reset();
	CHECK(net_port_alloc(20000, 19000, &p));
	CHECK_UINT(p, 10000);

	CASE("port: a range of one port allows exactly one call");
	net_port_reset();
	CHECK(net_port_alloc(20000, 20000, &p));
	CHECK_UINT(p, 20000);
	CHECK(!net_port_alloc(20000, 20000, &q));

	CASE("port: freeing a port outside the pool changes nothing");
	net_port_reset();
	CHECK(net_port_alloc(10000, 10030, &p));
	net_port_free(9000);
	net_port_free(0);
	CHECK(net_port_alloc(10000, 10030, &q));
	CHECK_UINT(q, 10002);

	CASE("port: the range is not rebased while a port is out");
	net_port_reset();
	CHECK(net_port_alloc(10000, 10030, &p));
	CHECK(net_port_alloc(30000, 30030, &q));
	CHECK_UINT(q, 10002);

	CASE("port: an empty pool adopts the new range");
	net_port_free(p);
	net_port_free(q);
	CHECK(net_port_alloc(30000, 30030, &r));
	CHECK_UINT(r, 30000);

	CASE("port: a NULL out parameter is refused");
	CHECK(!net_port_alloc(10000, 10030, NULL));
	net_port_reset();
}

/*
 * The effective range, which the firewall reconcile opens. It must be the
 * range that the allocator hands out. A rule that does not cover the pool
 * gives a call whose far end audio never arrives.
 */
static void test_port_range(void)
{
	struct cfg_channel ch = cfg.channel[0];
	uint16_t lo = 0, hi = 0, p = 0;

	CASE("port range: a configured pair is used as it stands");
	ch.rtp_port_min = 20000;
	ch.rtp_port_max = 20100;
	net_port_range(&ch, &lo, &hi);
	CHECK_UINT(lo, 20000);
	CHECK_UINT(hi, 20100);

	CASE("port range: an unset pair takes the built in range");
	ch.rtp_port_min = 0;
	ch.rtp_port_max = 0;
	net_port_range(&ch, &lo, &hi);
	CHECK_UINT(lo, 10000);
	CHECK_UINT(hi, 10030);

	CASE("port range: half a configured pair is incoherent");
	ch.rtp_port_min = 20000;
	ch.rtp_port_max = 0;
	net_port_range(&ch, &lo, &hi);
	CHECK_UINT(lo, 10000);
	CHECK_UINT(hi, 10030);
	ch.rtp_port_min = 0;
	ch.rtp_port_max = 20100;
	net_port_range(&ch, &lo, &hi);
	CHECK_UINT(lo, 10000);
	CHECK_UINT(hi, 10030);

	CASE("port range: an inverted pair takes the built in range");
	ch.rtp_port_min = 20100;
	ch.rtp_port_max = 20000;
	net_port_range(&ch, &lo, &hi);
	CHECK_UINT(lo, 10000);
	CHECK_UINT(hi, 10030);

	CASE("port range: a bound above 65535 takes the built in range");
	ch.rtp_port_min = 20000;
	ch.rtp_port_max = 70000;
	net_port_range(&ch, &lo, &hi);
	CHECK_UINT(lo, 10000);
	CHECK_UINT(hi, 10030);

	CASE("port range: no channel at all takes the built in range");
	net_port_range(NULL, &lo, &hi);
	CHECK_UINT(lo, 10000);
	CHECK_UINT(hi, 10030);

	CASE("port range: a NULL out parameter is tolerated");
	lo = 0;
	net_port_range(NULL, NULL, NULL);
	net_port_range(NULL, &lo, NULL);
	CHECK_UINT(lo, 10000);

	CASE("port range: the allocator hands out what this reports");
	ch.rtp_port_min = 20000;
	ch.rtp_port_max = 20100;
	net_port_reset();
	net_port_range(&ch, &lo, &hi);
	CHECK(net_port_alloc(lo, hi, &p));
	CHECK_UINT(p, 20000);

	CASE("port range: and takes the same fallback when the config is unset");
	ch.rtp_port_min = 0;
	ch.rtp_port_max = 0;
	net_port_reset();
	net_port_range(&ch, &lo, &hi);
	CHECK(net_port_alloc(lo, hi, &p));
	CHECK_UINT(p, 10000);
	net_port_reset();

	CASE("port range: a leg allocates inside the range the firewall opens");
	fx_reset();
	cfg.channel[0].rtp_port_min = 30000;
	cfg.channel[0].rtp_port_max = 30010;
	CHECK(call_dial(0, 0, "1002", true));
	net_port_range(&cfg.channel[0], &lo, &hi);
	CHECK_UINT(lo, 30000);
	CHECK_UINT(hi, 30010);
	CHECK(has(last_text(), "m=audio 30000"));
	cfg.channel[0].rtp_port_min = 10000;
	cfg.channel[0].rtp_port_max = 10030;

	/*
	 * rtp_local_port_min/max is RESTART_LINE, so a reload that moves the
	 * effective range clears the line before the rule moves: a live leg
	 * holds a port from the old range and the new rule would not cover it.
	 */
	CASE("port range: a moved range is a change, an unmoved one is not");
	{
		struct cfg_channel a = cfg.channel[0], b;

		a.rtp_port_min = 10000;
		a.rtp_port_max = 10030;
		b = a;
		CHECK(net_port_range_eq(&a, &b));
		b.rtp_port_max = 10040;
		CHECK(!net_port_range_eq(&a, &b));
		b = a;
		b.rtp_port_min = 10002;
		CHECK(!net_port_range_eq(&a, &b));
	}

	CASE("port range: two spellings of the same range are not a change");
	{
		struct cfg_channel a = cfg.channel[0], b = cfg.channel[0];

		a.rtp_port_min = 0;		/* unset */
		a.rtp_port_max = 0;
		b.rtp_port_min = 10000;		/* the same range, spelled out */
		b.rtp_port_max = 10030;
		CHECK(net_port_range_eq(&a, &b));

		b.rtp_port_min = 20000;		/* incoherent: half a pair */
		b.rtp_port_max = 0;
		CHECK(net_port_range_eq(&a, &b));

		CHECK(net_port_range_eq(NULL, &a));
		CHECK(net_port_range_eq(NULL, NULL));
	}

	CASE("port range: a cleared line lets the pool move to the new range");
	fx_reset();
	cfg.channel[0].rtp_port_min = 10000;
	cfg.channel[0].rtp_port_max = 10030;
	CHECK(call_dial(0, 0, "1002", true));
	CHECK(has(last_text(), "m=audio 10000"));

	/* the reload sequence for a moved range: clear the line, then reload */
	cfg.channel[0].rtp_port_min = 40000;
	cfg.channel[0].rtp_port_max = 40010;
	calls_line_teardown(0);
	calls_reload(&cfg);
	CHECK(call_dial(0, 0, "1003", true));
	CHECK(has(last_text(), "m=audio 40000"));

	CASE("port range: a live leg elsewhere keeps the pool where it is");
	fx_reset();
	cfg.channel[0].rtp_port_min = 10000;
	cfg.channel[0].rtp_port_max = 10030;
	CHECK(call_dial(0, 0, "1002", true));
	cfg.channel[0].rtp_port_min = 40000;
	cfg.channel[0].rtp_port_max = 40010;
	calls_reload(&cfg);		/* no teardown: nothing cleared the line */
	CHECK(call_dial(0, 1, "1003", true));
	CHECK(has(last_text(), "m=audio 10002"));
	cfg.channel[0].rtp_port_min = 10000;
	cfg.channel[0].rtp_port_max = 10030;
}

/* ----------------------------------------------------------- sdp_local */

/*
 * sdp.c decides which codec loses a contested payload type and keeps no
 * counter. call.c writes the one warning, so the cases below read it through
 * the capture of test_warn.h.
 */
static void test_sdp_local(void)
{
	struct sdp_local loc;
	struct cfg base;
	struct cfg_channel ch;
	unsigned int i;

	memset(&base, 0, sizeof(base));
	ch = cfg.channel[0];

	CASE("sdp_local: no codec section offers the three schema codecs in order");
	media_sdp_local_build(&base, &ch, ~0u, T_HOST, 10000, &loc);
	CHECK_UINT(loc.codecs, 3);
	CHECK_UINT(loc.codec[0].id, SDP_CODEC_PCMU);
	CHECK_UINT(loc.codec[0].pt, 0);
	CHECK_UINT(loc.codec[1].id, SDP_CODEC_PCMA);
	CHECK_UINT(loc.codec[1].pt, 8);
	CHECK_UINT(loc.codec[2].id, SDP_CODEC_G729);
	CHECK_UINT(loc.codec[2].pt, 18);

	CASE("sdp_local: the address and the port come from the leg");
	CHECK(!strcmp(loc.addr, T_HOST));
	CHECK_UINT(loc.port, 10000);

	CASE("sdp_local: RFC 2833 offers telephone-event 101");
	CHECK_UINT(loc.dtmf_pt, SDP_DTMF_PT_DEFAULT);

	CASE("sdp_local: inband dtmf offers no telephone-event");
	ch.dtmf_mode = CFG_DTMF_INBAND;
	media_sdp_local_build(&base, &ch, ~0u, T_HOST, 10000, &loc);
	CHECK_UINT(loc.dtmf_pt, 0);

	CASE("sdp_local: info dtmf offers no telephone-event either");
	ch.dtmf_mode = CFG_DTMF_INFO;
	media_sdp_local_build(&base, &ch, ~0u, T_HOST, 10000, &loc);
	CHECK_UINT(loc.dtmf_pt, 0);
	ch.dtmf_mode = CFG_DTMF_RFC2833;

	CASE("sdp_local: payload type 129 disables the codec");
	ch.g711a_ptype = SDP_PT_DISABLED;
	media_sdp_local_build(&base, &ch, ~0u, T_HOST, 10000, &loc);
	CHECK_UINT(loc.codecs, 2);
	CHECK_UINT(loc.codec[0].id, SDP_CODEC_PCMU);
	CHECK_UINT(loc.codec[1].id, SDP_CODEC_G729);
	ch.g711a_ptype = 8;

	CASE("sdp_local: the capability mask removes what the build cannot do");
	media_sdp_local_build(&base, &ch, 1u << HAL_CODEC_PCMU, T_HOST, 10000, &loc);
	CHECK_UINT(loc.codecs, 1);
	CHECK_UINT(loc.codec[0].id, SDP_CODEC_PCMU);

	CASE("sdp_local: an empty mask leaves nothing to offer");
	media_sdp_local_build(&base, &ch, 0, T_HOST, 10000, &loc);
	CHECK_UINT(loc.codecs, 0);

	CASE("sdp_local: codec sections keep their configured order");
	base.codecs = 2;
	base.codec[0].enabled = true;
	base.codec[0].tr104_id = 2;
	base.codec[1].enabled = true;
	base.codec[1].tr104_id = 1;
	media_sdp_local_build(&base, &ch, ~0u, T_HOST, 10000, &loc);
	CHECK_UINT(loc.codecs, 2);
	CHECK_UINT(loc.codec[0].id, SDP_CODEC_PCMA);
	CHECK_UINT(loc.codec[1].id, SDP_CODEC_PCMU);

	CASE("sdp_local: a disabled section is skipped but still counts as one");
	base.codec[1].enabled = false;
	media_sdp_local_build(&base, &ch, ~0u, T_HOST, 10000, &loc);
	CHECK_UINT(loc.codecs, 1);
	CHECK_UINT(loc.codec[0].id, SDP_CODEC_PCMA);
	base.codec[1].enabled = true;

	CASE("sdp_local: silence suppression rides the codec section");
	base.codec[0].silence_suppression = true;
	media_sdp_local_build(&base, &ch, ~0u, T_HOST, 10000, &loc);
	CHECK(loc.codec[0].vad);
	CHECK(!loc.codec[1].vad);
	base.codec[0].silence_suppression = false;

	CASE("sdp_local: G.726 takes the configured payload type");
	base.codecs = 3;
	base.codec[2].enabled = true;
	base.codec[2].tr104_id = 4;
	ch.g726_ptype = 120;
	media_sdp_local_build(&base, &ch, ~0u, T_HOST, 10000, &loc);
	CHECK_UINT(loc.codecs, 3);
	CHECK_UINT(loc.codec[2].id, SDP_CODEC_G726_32);
	CHECK_UINT(loc.codec[2].pt, 120);

	CASE("sdp_local: a G.726 payload type of 129 skips the codec");
	ch.g726_ptype = SDP_PT_DISABLED;
	media_sdp_local_build(&base, &ch, ~0u, T_HOST, 10000, &loc);
	CHECK_UINT(loc.codecs, 2);

	CASE("sdp_local: G.726 needs its HAL codec bit");
	ch.g726_ptype = 120;
	media_sdp_local_build(&base, &ch,
		~0u & ~(1u << HAL_CODEC_G726_32), T_HOST, 10000, &loc);
	CHECK_UINT(loc.codecs, 2);
	ch.g726_ptype = 0;

	CASE("sdp_local: a section for another channel does not apply");
	base.codec[0].channel = 2;
	base.codec[1].channel = 2;
	base.codec[2].channel = 2;
	media_sdp_local_build(&base, &ch, ~0u, T_HOST, 10000, &loc);
	CHECK_UINT(loc.codecs, 3);
	CHECK_UINT(loc.codec[0].id, SDP_CODEC_PCMU);

	CASE("sdp_local: a repeated codec is offered once");
	base.codec[0].channel = 0;
	base.codec[1].channel = 0;
	base.codec[2].channel = 0;
	base.codec[2].tr104_id = 2;
	media_sdp_local_build(&base, &ch, ~0u, T_HOST, 10000, &loc);
	CHECK_UINT(loc.codecs, 2);

	CASE("sdp_local: ptime and CN come from the channel");
	ch.ptime_ms = 30;
	ch.cn_enable = true;
	media_sdp_local_build(&base, &ch, ~0u, T_HOST, 10000, &loc);
	CHECK_UINT(loc.ptime_ms, 30);
	CHECK(loc.cn);

	CASE("sdp_local: an unset ptime falls back to 20 ms");
	ch.ptime_ms = 0;
	media_sdp_local_build(&base, &ch, ~0u, T_HOST, 10000, &loc);
	CHECK_UINT(loc.ptime_ms, 20);

	/*
	 * The wideband rows. tr104_id 5 and 6 reach the list through a named
	 * section alone, so the default list above is what a line with no
	 * section still offers even on a HAL that advertises G.722.
	 */
	memset(&base, 0, sizeof(base));
	ch = cfg.channel[0];

	CASE("sdp_local: the default list is unchanged on a HAL that carries G722");
	media_sdp_local_build(&base, &ch, ~0u, T_HOST, 10000, &loc);
	CHECK_UINT(loc.codecs, 3);
	CHECK_UINT(loc.codec[0].id, SDP_CODEC_PCMU);
	CHECK_UINT(loc.codec[1].id, SDP_CODEC_PCMA);
	CHECK_UINT(loc.codec[2].id, SDP_CODEC_G729);

	CASE("sdp_local: a tr104 5 section offers G722 at payload type 9");
	base.codecs = 1;
	base.codec[0].enabled = true;
	base.codec[0].tr104_id = 5;
	CHECK_UINT(ch.g722_ptype, 0);
	media_sdp_local_build(&base, &ch, ~0u, T_HOST, 10000, &loc);
	CHECK_UINT(loc.codecs, 1);
	CHECK_UINT(loc.codec[0].id, SDP_CODEC_G722);
	CHECK_UINT(loc.codec[0].pt, 9);

	CASE("sdp_local: g722_ptype 129 disables the configured G722");
	ch.g722_ptype = SDP_PT_DISABLED;
	media_sdp_local_build(&base, &ch, ~0u, T_HOST, 10000, &loc);
	CHECK_UINT(loc.codecs, 0);
	ch.g722_ptype = 0;

	CASE("sdp_local: a HAL without G722 refuses the configured section");
	media_sdp_local_build(&base, &ch, ~0u & ~(1u << HAL_CODEC_G722), T_HOST, 10000, &loc);
	CHECK_UINT(loc.codecs, 0);

	CASE("sdp_local: G722 keeps the section order ahead of PCMU");
	base.codecs = 2;
	base.codec[1].enabled = true;
	base.codec[1].tr104_id = 1;
	media_sdp_local_build(&base, &ch, ~0u, T_HOST, 10000, &loc);
	CHECK_UINT(loc.codecs, 2);
	CHECK_UINT(loc.codec[0].id, SDP_CODEC_G722);
	CHECK_UINT(loc.codec[1].id, SDP_CODEC_PCMU);

	/*
	 * tr104_id 6 carries the Opus row, sdp.c carries the SDP side, and the
	 * stub and udsp backends advertise HAL_CODEC_OPUS, so a real build
	 * reaches this arm on a configured section. The mask case below models
	 * a backend that maps no Opus bit.
	 */
	CASE("sdp_local: a tr104 6 section names Opus at the configured payload type");
	base.codecs = 1;
	base.codec[0].tr104_id = 6;
	CHECK_UINT(ch.opus_ptype, 0);
	media_sdp_local_build(&base, &ch, ~0u, T_HOST, 10000, &loc);
	CHECK_UINT(loc.codecs, 1);
	CHECK_UINT(loc.codec[0].id, SDP_CODEC_OPUS);
	CHECK_UINT(loc.codec[0].pt, 96);

	CASE("sdp_local: opus_ptype rides the channel option");
	ch.opus_ptype = 110;
	media_sdp_local_build(&base, &ch, ~0u, T_HOST, 10000, &loc);
	CHECK_UINT(loc.codec[0].pt, 110);
	ch.opus_ptype = SDP_PT_DISABLED;
	media_sdp_local_build(&base, &ch, ~0u, T_HOST, 10000, &loc);
	CHECK_UINT(loc.codecs, 0);
	ch.opus_ptype = 0;

	CASE("sdp_local: a HAL without Opus refuses the configured section");
	media_sdp_local_build(&base, &ch, ~0u & ~(1u << HAL_CODEC_OPUS), T_HOST, 10000, &loc);
	CHECK_UINT(loc.codecs, 0);

	/*
	 * The offerability gate. The list and the offer run one predicate. A
	 * payload type past 127 sets it apart from a check for the disabled
	 * value 129 alone. That check would seat an entry for 128 that
	 * sdp_offer_build() skips, so the line's local view would name a codec
	 * that the wire never carries.
	 */
	CASE("sdp_local: a payload type past 127 seats no Opus entry");
	ch.opus_ptype = 128;
	media_sdp_local_build(&base, &ch, ~0u, T_HOST, 10000, &loc);
	CHECK_UINT(loc.codecs, 0);
	ch.opus_ptype = 0;

	CASE("sdp_local: the same gate holds for a narrowband codec");
	base.codec[0].tr104_id = 2;
	ch.g711a_ptype = 200;
	media_sdp_local_build(&base, &ch, ~0u, T_HOST, 10000, &loc);
	CHECK_UINT(loc.codecs, 0);
	ch.g711a_ptype = 0;
	base.codec[0].tr104_id = 6;

	CASE("sdp_local: every entry the default list seats passes that gate");
	media_sdp_local_build(&base, &cfg.channel[0], ~0u, T_HOST, 10000, &loc);
	CHECK_UINT(loc.codecs, 1);
	for (i = 0; i < loc.codecs; i++)
		CHECK(sdp_codec_offerable(&loc.codec[i]));

	/*
	 * The Opus encoder ask. bitrate, fec and complexity belong to the codec
	 * section, they never reach the wire, and they ride the negotiated
	 * entry so media_fill() reads them beside the peer's constraints.
	 * Every other codec leaves the block zero.
	 */
	CASE("sdp_local: an Opus section carries its encoder ask into the entry");
	base.codec[0].bitrate = 24000;
	base.codec[0].fec = true;
	base.codec[0].complexity = 7;
	media_sdp_local_build(&base, &ch, ~0u, T_HOST, 10000, &loc);
	CHECK_UINT(loc.codecs, 1);
	CHECK_UINT(loc.codec[0].opus.bitrate, 24000);
	CHECK_UINT(loc.codec[0].opus.complexity, 7);
	CHECK(loc.codec[0].opus.fec);

	CASE("sdp_local: an unset bitrate and complexity stay the library default");
	base.codec[0].bitrate = 0;
	base.codec[0].complexity = 0;
	media_sdp_local_build(&base, &ch, ~0u, T_HOST, 10000, &loc);
	CHECK_UINT(loc.codec[0].opus.bitrate, 0);
	CHECK_UINT(loc.codec[0].opus.complexity, 0);

	CASE("sdp_local: the same section on G722 carries no encoder ask");
	base.codec[0].tr104_id = 5;
	base.codec[0].bitrate = 24000;
	base.codec[0].fec = true;
	base.codec[0].complexity = 7;
	media_sdp_local_build(&base, &ch, ~0u, T_HOST, 10000, &loc);
	CHECK_UINT(loc.codec[0].id, SDP_CODEC_G722);
	CHECK_UINT(loc.codec[0].opus.bitrate, 0);
	CHECK_UINT(loc.codec[0].opus.complexity, 0);
	CHECK(!loc.codec[0].opus.fec);
	base.codec[0].bitrate = 0;
	base.codec[0].fec = false;
	base.codec[0].complexity = 0;

	/*
	 * The drop warning. One line per dropped codec, naming the codec that
	 * loses the payload type and the rtpmap line that keeps it, and nothing
	 * at all on a config with no collision.
	 */
	memset(&base, 0, sizeof(base));
	ch = cfg.channel[0];

	CASE("sdp_local: a clean codec list logs no drop");
	warn_capture_begin();
	media_sdp_local_build(&base, &ch, ~0u, T_HOST, 10000, &loc);
	CHECK_UINT(loc.codecs, 3);
	CHECK_UINT(warn_capture_count("drops codec"), 0);

	CASE("sdp_local: a codec that loses a payload type is logged once, naming both");
	base.codecs = 2;
	base.codec[0].enabled = true;
	base.codec[0].tr104_id = 2;
	base.codec[1].enabled = true;
	base.codec[1].tr104_id = 5;
	ch.g722_ptype = 8;
	warn_capture_begin();
	media_sdp_local_build(&base, &ch, ~0u, T_HOST, 10000, &loc);
	CHECK_UINT(loc.codecs, 2);
	CHECK_UINT(warn_capture_count(
		"line 0 drops codec G722 from the offer: payload type 8 already carries PCMA"), 1);
	CHECK_UINT(warn_capture_count("drops codec"), 1);
	ch.g722_ptype = 0;

	CASE("sdp_local: a codec on the telephone-event type names telephone-event");
	base.codecs = 1;
	base.codec[0].tr104_id = 3;
	ch.g729a_ptype = SDP_DTMF_PT_DEFAULT;
	warn_capture_begin();
	media_sdp_local_build(&base, &ch, ~0u, T_HOST, 10000, &loc);
	CHECK_UINT(warn_capture_count(
		"line 0 drops codec G729 from the offer: payload type 101 already carries telephone-event"), 1);
	CHECK_UINT(warn_capture_count("drops codec"), 1);
	ch.g729a_ptype = 0;

	CASE("sdp_local: a codec on the comfort noise type names CN");
	base.codecs = 1;
	base.codec[0].tr104_id = 5;
	ch.cn_enable = true;
	ch.g722_ptype = 13;		/* RFC 3551 table 4: comfort noise */
	warn_capture_begin();
	media_sdp_local_build(&base, &ch, ~0u, T_HOST, 10000, &loc);
	CHECK_UINT(warn_capture_count(
		"line 0 drops codec G722 from the offer: payload type 13 already carries CN"), 1);
	CHECK_UINT(warn_capture_count("drops codec"), 1);
	ch.g722_ptype = 0;
	ch.cn_enable = false;

	CASE("sdp_local: two dropped codecs are logged once each");
	base.codecs = 3;
	base.codec[0].tr104_id = 1;
	base.codec[1].enabled = true;
	base.codec[1].tr104_id = 2;
	base.codec[2].enabled = true;
	base.codec[2].tr104_id = 3;
	ch.g711a_ptype = SDP_DTMF_PT_DEFAULT;
	ch.g729a_ptype = SDP_DTMF_PT_DEFAULT;
	warn_capture_begin();
	media_sdp_local_build(&base, &ch, ~0u, T_HOST, 10000, &loc);
	CHECK_UINT(warn_capture_count("drops codec PCMA"), 1);
	CHECK_UINT(warn_capture_count("drops codec G729"), 1);
	CHECK_UINT(warn_capture_count("drops codec"), 2);
	warn_capture_end();
	ch.g711a_ptype = 0;
	ch.g729a_ptype = 0;

	CASE("sdp_local: a NULL channel yields nothing");
	memset(&base, 0, sizeof(base));
	media_sdp_local_build(&base, NULL, ~0u, T_HOST, 10000, &loc);
	CHECK_UINT(loc.codecs, 0);
}

/* ---------------------------------------------------------- hal_media */

static void test_media_fill(void)
{
	struct cfg_channel ch = cfg.channel[0];
	struct hal_media m, prev;
	struct sdp_result res;

	memset(&res, 0, sizeof(res));
	res.codec.id = SDP_CODEC_PCMA;
	res.codec.pt = 8;
	res.dtmf_pt = 101;
	res.cn_pt = -1;
	res.dir = SDP_SENDRECV;
	res.ptime_ms = 20;
	res.remote_port = 20000;
	res.remote_ipver = 4;
	res.valid = true;
	snprintf(res.remote_addr, sizeof(res.remote_addr), "%s", T_PEER);

	CASE("media: the codec maps onto the HAL vocabulary");
	media_fill(&ch, true, 5, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.codec, HAL_CODEC_PCMA);
	CHECK_UINT(m.codec_pt, 8);

	CASE("media: a TOS byte becomes a DSCP");
	CHECK_UINT(m.rtp_dscp, 46);
	CHECK_UINT(m.rtcp_dscp, 46);

	CASE("media: the local address carries the RTCP port above the RTP one");
	CHECK_UINT(m.local.ipver, 4);
	CHECK_UINT(m.local.port, 10000);
	CHECK_UINT(m.local.rtcp_port, 10001);

	CASE("media: the remote address comes from the negotiation");
	CHECK_UINT(m.remote.port, 20000);
	CHECK_UINT(m.remote.rtcp_port, 20001);
	CHECK_UINT(m.remote.ipver, 4);
	CHECK_UINT(m.remote.addr[0], 198);

	CASE("media: RFC 2833 stays when the payload type was negotiated");
	CHECK_UINT(m.dtmf_mode, HAL_DTMF_RFC2833);
	CHECK_UINT(m.dtmf_pt, 101);

	CASE("media: a refused telephone-event downgrades to inband");
	res.dtmf_pt = -1;
	media_fill(&ch, true, 5, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.dtmf_mode, HAL_DTMF_INBAND);
	CHECK_UINT(m.dtmf_pt, 0);
	res.dtmf_pt = 101;

	CASE("media: the jitter buffer and the bind device come from the arguments");
	media_fill(&ch, true, 5, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.jb_mode, HAL_JB_ADAPTIVE);
	CHECK_UINT(m.jb_init_ms, 50);
	/* the default ceiling of the adaptive jitter buffer is 250 ms */
	CHECK_UINT(m.jb_max_ms, 250);
	CHECK(!strcmp(m.bind_dev, "wan"));

	/* The media punch cadence travels as the leg took it from its view
	 * (hal.h, hal_media.punch_interval_ms); 0 leaves the backend
	 * default. */
	CASE("media: the punch cadence is written as the argument stands");
	CHECK_UINT(m.punch_interval_ms, 0);
	media_fill(&ch, true, 5, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 15000, &m);
	CHECK_UINT(m.punch_interval_ms, 15000);
	media_fill(&ch, true, 5, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 15, &m);
	CHECK_UINT(m.punch_interval_ms, 15);
	CHECK_UINT(m.remote.port, 20000);

	CASE("media: an explicit RTCP interval stays unchanged");
	CHECK(m.rtcp_enable);
	CHECK_UINT(m.rtcp_interval_ms, 5000);

	CASE("media: enabled RTCP with no interval defaults to five seconds");
	media_fill(&ch, true, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK(m.rtcp_enable);
	CHECK_UINT(m.rtcp_interval_ms, 5000);

	CASE("media: disabled RTCP keeps a zero interval");
	media_fill(&ch, false, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK(!m.rtcp_enable);
	CHECK_UINT(m.rtcp_interval_ms, 0);

	CASE("media: every direction maps");
	res.dir = SDP_SENDONLY;
	media_fill(&ch, true, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.direction, HAL_MEDIA_SENDONLY);
	res.dir = SDP_RECVONLY;
	media_fill(&ch, true, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.direction, HAL_MEDIA_RECVONLY);
	res.dir = SDP_INACTIVE;
	media_fill(&ch, true, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.direction, HAL_MEDIA_INACTIVE);
	res.dir = SDP_SENDRECV;

	CASE("media: CN and VAD ride the result");
	res.cn_pt = 13;
	res.codec.vad = true;
	media_fill(&ch, true, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK(m.cng);
	CHECK(m.vad);
	res.cn_pt = -1;
	res.codec.vad = false;

	/*
	 * The negotiated Annex B (hal.h, hal_media.annexb). It is the
	 * negotiation's own answer and not the codec section's ask, so it
	 * travels on its own; an armed exchange keeps the vad ask beside it,
	 * and the udsp engine is what forces the generic pair off behind the
	 * dtx.
	 */
	CASE("media: the negotiated Annex B rides the result onto the description");
	res.codec.id = SDP_CODEC_G729;
	res.codec.annexb = true;
	res.codec.vad = true;
	media_fill(&ch, true, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK(m.annexb);
	CHECK(m.vad);

	/* G.729 suppresses silence through Annex B or not at all: an unarmed
	 * exchange drops the vad ask here, whatever comfort noise says, and
	 * the comfort noise answer itself still rides. */
	CASE("media: an unarmed G.729 exchange clears the vad ask");
	res.codec.annexb = false;
	media_fill(&ch, true, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK(!m.annexb);
	CHECK(!m.vad);
	res.cn_pt = 13;
	media_fill(&ch, true, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK(!m.vad);
	CHECK(m.cng);
	res.cn_pt = -1;

	CASE("media: the vad ask stands on a codec with no Annex B");
	res.codec.id = SDP_CODEC_PCMA;
	media_fill(&ch, true, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK(!m.annexb);
	CHECK(m.vad);
	res.codec.vad = false;

	CASE("media: an unset ptime falls back to 20 ms");
	res.ptime_ms = 0;
	media_fill(&ch, true, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.ptime_ms, 20);
	res.ptime_ms = 20;

	CASE("media: G.729 and PCMU map too");
	res.codec.id = SDP_CODEC_G729;
	media_fill(&ch, true, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.codec, HAL_CODEC_G729);
	res.codec.id = SDP_CODEC_PCMU;
	media_fill(&ch, true, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.codec, HAL_CODEC_PCMU);

	CASE("Negotiated G.726 programs the 32 kbit codec");
	res.codec.id = SDP_CODEC_G726_32;
	res.codec.pt = 111;
	media_fill(&ch, true, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.codec, HAL_CODEC_G726_32);
	CHECK_UINT(m.codec_pt, 111);

	/*
	 * The negotiated G.722 pin. The media record carries no clock rate: the
	 * backend derives the 16 kHz audio from the codec alone, so this one
	 * field is what programs a wideband stream. Comfort noise still rides
	 * payload type 13 on the 8000 RTP clock beside it.
	 */
	CASE("media: a negotiated G722 programs the wideband codec");
	res.codec.id = SDP_CODEC_G722;
	res.codec.pt = 9;
	res.cn_pt = 13;
	media_fill(&ch, true, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.codec, HAL_CODEC_G722);
	CHECK_UINT(m.codec_pt, 9);
	CHECK_UINT(m.dtmf_pt, 101);
	CHECK(m.cng);
	CHECK_UINT(m.ptime_ms, 20);

	CASE("media: a G722 answered on a dynamic payload type keeps that type");
	res.codec.pt = 96;
	media_fill(&ch, true, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.codec, HAL_CODEC_G722);
	CHECK_UINT(m.codec_pt, 96);
	res.cn_pt = -1;
	res.codec.id = SDP_CODEC_PCMU;
	res.codec.pt = 8;

	/*
	 * The negotiated Opus. The record carries no clock rate here either:
	 * the codec is the whole key and the backend derives the 48000 RTP
	 * clock from it.
	 *
	 * The encoder is the configured ask narrowed by the peer's a=fmtp. Each
	 * case below moves one remote key and reads the one field it lands in.
	 */
	res.codec.id = SDP_CODEC_OPUS;
	res.codec.pt = 96;
	res.codec.opus.bitrate = 32000;
	res.codec.opus.complexity = 5;
	res.codec.opus.fec = true;

	CASE("media: a negotiated Opus programs the codec and the configured encoder");
	media_fill(&ch, true, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.codec, HAL_CODEC_OPUS);
	CHECK_UINT(m.codec_pt, 96);
	CHECK_UINT(m.opus.bitrate, 32000);
	CHECK_UINT(m.opus.complexity, 5);
	CHECK(m.opus.fec);
	CHECK_UINT(m.opus.max_bandwidth, 0);

	/* Opus pairs with neither silence suppression nor CN: the fill clears
	 * the vad flag and sdp.c withholds cn_pt from every Opus exchange */
	CASE("media: silence suppression is forced off on an Opus stream");
	res.codec.vad = true;
	media_fill(&ch, true, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK(!m.vad);
	CHECK(!m.cng);
	res.codec.vad = false;

	CASE("media: the same flag survives on the narrowband codec beside it");
	res.codec.id = SDP_CODEC_PCMU;
	res.codec.vad = true;
	media_fill(&ch, true, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK(m.vad);
	res.codec.vad = false;
	res.codec.id = SDP_CODEC_OPUS;

	CASE("media: a remote maxaveragebitrate below ours caps the encoder");
	res.opus_remote.maxaveragebitrate = 20000;
	media_fill(&ch, true, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.opus.bitrate, 20000);

	CASE("media: a remote maxaveragebitrate above ours does not raise it");
	res.opus_remote.maxaveragebitrate = 64000;
	media_fill(&ch, true, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.opus.bitrate, 32000);

	/* 0 means the library default, which is free to run above their cap */
	CASE("media: a remote cap replaces a configured bitrate of zero");
	res.codec.opus.bitrate = 0;
	res.opus_remote.maxaveragebitrate = 24000;
	media_fill(&ch, true, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.opus.bitrate, 24000);
	res.codec.opus.bitrate = 32000;
	res.opus_remote.maxaveragebitrate = 0;

	CASE("media: a remote useinbandfec of 0 disables our FEC");
	res.opus_remote.fec_present = true;
	res.opus_remote.fec = false;
	media_fill(&ch, true, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK(!m.opus.fec);

	CASE("media: a remote useinbandfec of 1 leaves it as configured");
	res.opus_remote.fec = true;
	media_fill(&ch, true, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK(m.opus.fec);

	CASE("media: a remote useinbandfec of 1 cannot switch our FEC on");
	res.codec.opus.fec = false;
	media_fill(&ch, true, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK(!m.opus.fec);
	res.codec.opus.fec = true;
	res.opus_remote.fec_present = false;
	res.opus_remote.fec = false;

	/* RFC 7587 6.1: the encoder must not run above the rate the peer renders */
	CASE("media: a remote maxplaybackrate picks the encoder bandwidth");
	res.opus_remote.maxplaybackrate = 8000;
	media_fill(&ch, true, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.opus.max_bandwidth, 1);
	res.opus_remote.maxplaybackrate = 12000;
	media_fill(&ch, true, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.opus.max_bandwidth, 2);
	res.opus_remote.maxplaybackrate = 16000;
	media_fill(&ch, true, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.opus.max_bandwidth, 3);
	res.opus_remote.maxplaybackrate = 48000;
	media_fill(&ch, true, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.opus.max_bandwidth, 3);
	res.opus_remote.maxplaybackrate = 0;

	/*
	 * A live re-INVITE that moves the remote fmtp. media_program()
	 * re-configures the stream on `memcmp(&m, &c->media, sizeof(m)) != 0`
	 * and on nothing else, so the description a changed constraint set
	 * produces must differ byte for byte from the one before it; the udsp
	 * side recreates the codec pair off exactly these parameters, and a
	 * description that compared equal would leave the encoder on the first
	 * exchange's terms with nothing on the wire to say so.
	 *
	 * Both fills run the same memset-then-fill discipline media_program()
	 * depends on, so the padding compares equal and the comparison below is
	 * the same one the programmer makes.
	 */
	CASE("media: the first exchange programs the peer's first constraint set");
	res.opus_remote.maxaveragebitrate = 48000;
	res.opus_remote.maxplaybackrate = 16000;
	res.opus_remote.fec_present = true;
	res.opus_remote.fec = true;
	media_fill(&ch, true, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &prev);
	CHECK_UINT(prev.opus.bitrate, 32000);
	CHECK_UINT(prev.opus.max_bandwidth, 3);
	CHECK(prev.opus.fec);

	CASE("media: a re-INVITE that repeats the same fmtp programs the same bytes");
	media_fill(&ch, true, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(memcmp(&m, &prev, sizeof(m)), 0);

	CASE("media: a re-INVITE that moves the fmtp reaches the encoder again");
	res.opus_remote.maxaveragebitrate = 16000;
	res.opus_remote.maxplaybackrate = 8000;
	res.opus_remote.fec = false;
	media_fill(&ch, true, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.opus.bitrate, 16000);
	CHECK_UINT(m.opus.max_bandwidth, 1);
	CHECK(!m.opus.fec);

	CASE("media: and the two descriptions differ, so the programmer re-applies");
	CHECK(memcmp(&m, &prev, sizeof(m)) != 0);

	CASE("media: a re-INVITE that drops the fmtp returns to the configured ask");
	memset(&res.opus_remote, 0, sizeof(res.opus_remote));
	media_fill(&ch, true, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.opus.bitrate, 32000);
	CHECK_UINT(m.opus.max_bandwidth, 0);
	CHECK(m.opus.fec);
	CHECK(memcmp(&m, &prev, sizeof(m)) != 0);

	CASE("media: an exchange on another codec programs no encoder at all");
	res.codec.id = SDP_CODEC_PCMU;
	res.opus_remote.maxaveragebitrate = 20000;
	media_fill(&ch, true, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.opus.bitrate, 0);
	CHECK(!m.opus.fec);
	res.opus_remote.maxaveragebitrate = 0;

	/*
	 * A peer that named no telephone-event/48000 leaves the Opus stream
	 * with no relay payload type: SIP INFO stays when the channel
	 * configured it and RFC 2833 falls back to inband. Inband DTMF over
	 * Opus is unreliable, so that fallback is counted and reported. The
	 * narrowband refusal stays silent.
	 *
	 * The report is not this entry's. The fill runs twice for one
	 * exchange, so the count and the warning moved to media_program(),
	 * which knows a new description from a repeat; the leg cases of
	 * test_codec_mask() pin them there. The three cases below therefore
	 * assert the fill's own answer and that it says nothing.
	 */
	res.codec.id = SDP_CODEC_OPUS;
	res.dtmf_pt = -1;

	CASE("media: a refused wideband telephone-event falls back to inband");
	warn_capture_begin();
	media_fill(&ch, true, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.dtmf_mode, HAL_DTMF_INBAND);
	CHECK_UINT(m.dtmf_pt, 0);
	CHECK_UINT(warn_capture_count("telephone-event/48000"), 0);

	CASE("media: the SIP INFO channel keeps INFO and the fill still says nothing");
	ch.dtmf_mode = CFG_DTMF_INFO;
	warn_capture_begin();
	media_fill(&ch, true, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.dtmf_mode, HAL_DTMF_INFO);
	CHECK_UINT(warn_capture_count("telephone-event/48000"), 0);

	CASE("media: an inband channel had nothing to fall back from and says nothing");
	ch.dtmf_mode = CFG_DTMF_INBAND;
	warn_capture_begin();
	media_fill(&ch, true, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.dtmf_mode, HAL_DTMF_INBAND);
	CHECK_UINT(warn_capture_count("telephone-event/48000"), 0);
	ch.dtmf_mode = CFG_DTMF_RFC2833;

	CASE("media: the narrowband refusal keeps its silence");
	res.codec.id = SDP_CODEC_PCMU;
	warn_capture_begin();
	media_fill(&ch, true, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.dtmf_mode, HAL_DTMF_INBAND);
	CHECK_UINT(warn_capture_count("telephone-event/48000"), 0);

	CASE("media: a negotiated wideband telephone-event warns about nothing");
	res.codec.id = SDP_CODEC_OPUS;
	res.dtmf_pt = 102;
	warn_capture_begin();
	media_fill(&ch, true, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.dtmf_mode, HAL_DTMF_RFC2833);
	CHECK_UINT(m.dtmf_pt, 102);
	CHECK_UINT(warn_capture_count("telephone-event/48000"), 0);

	memset(&res.codec.opus, 0, sizeof(res.codec.opus));
	memset(&res.opus_remote, 0, sizeof(res.opus_remote));
	res.dtmf_pt = 101;
	res.codec.id = SDP_CODEC_PCMU;
	res.codec.pt = 8;

	CASE("media: the SIP INFO mode survives a negotiated payload type");
	ch.dtmf_mode = CFG_DTMF_INFO;
	media_fill(&ch, true, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.dtmf_mode, HAL_DTMF_INFO);

	CASE("media: a NULL result leaves the description empty");
	media_fill(&ch, true, 0, false, NULL, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.codec, HAL_CODEC_NONE);
	CHECK_UINT(m.local.port, 0);

	/*
	 * The fax override. These four fields are the ones a fax carrier
	 * cannot survive, and this fill is the one place every stream
	 * programming passes through: without the override the first later
	 * exchange of a converted call reverts them to the negotiated values.
	 * The cases drive the fill with a result that asks for all three, which
	 * is what a channel with silence suppression on its G.711 row and a
	 * comfort noise answer produces.
	 */
	ch.dtmf_mode = CFG_DTMF_RFC2833;
	res.codec.id = SDP_CODEC_PCMU;
	res.cn_pt = 13;
	res.codec.vad = true;
	/* The Annex B result joins the three: a data carrier has no talkspurt,
	 * so the codec's own detector cannot survive a fax either. It rides a
	 * G.711 result here because the override reads no codec at all. */
	res.codec.annexb = true;

	CASE("media fill: a voice stream takes the negotiated three as they are");
	media_fill(&ch, true, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.jb_mode, HAL_JB_ADAPTIVE);
	CHECK(m.vad);
	CHECK(m.cng);
	CHECK(m.annexb);

	CASE("media fill: and carries no passthrough kind at all");
	CHECK_UINT(m.passthrough, HAL_PASSTHROUGH_OFF);

	CASE("media fill: a fax stream holds the jitter buffer at the fixed mode");
	media_fill(&ch, true, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_MODEM, 0, &m);
	CHECK_UINT(m.jb_mode, HAL_JB_FIXED);

	CASE("media fill: a fax stream carries no silence suppression, even when "
	     "the codec section asked for it");
	CHECK(!m.vad);

	CASE("media fill: and no comfort noise, even when the answer named a "
	     "payload type for it");
	CHECK(!m.cng);

	CASE("media fill: and no codec-native DTX, even when the exchange armed it");
	CHECK(!m.annexb);

	/* The kind is the fourth field and the one the backend reads for its
	 * canceller policy: the leg's kind travels as it stands, so the
	 * description says modem or fax and not merely "a fax". */
	CASE("media fill: and the modem kind reaches the description");
	CHECK_UINT(m.passthrough, HAL_PASSTHROUGH_MODEM);

	CASE("media fill: the fax kind reaches it with the same three overrides");
	media_fill(&ch, true, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_FAX, 0, &m);
	CHECK_UINT(m.passthrough, HAL_PASSTHROUGH_FAX);
	CHECK_UINT(m.jb_mode, HAL_JB_FIXED);
	CHECK(!m.vad);
	CHECK(!m.cng);
	CHECK(!m.annexb);

	/* the sizes stay the ones the call negotiated: the backend edits the
	 * mode alone for the same reason (hal.h jb_fixed) */
	CASE("media fill: and the negotiated buffer sizes are left alone");
	CHECK_UINT(m.jb_init_ms, 50);
	/* the default ceiling of the adaptive jitter buffer is 250 ms */
	CHECK_UINT(m.jb_max_ms, 250);

	CASE("media fill: and nothing else of the description moves");
	CHECK_UINT(m.codec, HAL_CODEC_PCMU);
	CHECK_UINT(m.dtmf_pt, 101);
	CHECK_UINT(m.direction, HAL_MEDIA_SENDRECV);
	CHECK_UINT(m.remote.port, 20000);

	res.cn_pt = -1;
	res.codec.vad = false;
	res.codec.annexb = false;

	/*
	 * The address the fill refuses. A remote description the media layer
	 * cannot honor leaves an empty description, and that is what carries
	 * the refusal to media_program(). The wire cases live in
	 * test_media_addr().
	 */
	CASE("media addr: a c= line that is not a literal empties the description");
	snprintf(res.remote_addr, sizeof(res.remote_addr), "sip.example.com");
	media_fill(&ch, true, 5, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.remote.port, 0);
	CHECK_UINT(m.local.port, 0);
	CHECK_UINT(m.codec, HAL_CODEC_NONE);
	CHECK_UINT(m.rtcp_interval_ms, 0);

	CASE("media addr: the RFC 2543 0.0.0.0 hold form is a literal and "
	     "fills");
	snprintf(res.remote_addr, sizeof(res.remote_addr), "0.0.0.0");
	media_fill(&ch, true, 5, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.remote.ipver, 4);
	CHECK_UINT(m.remote.addr[0], 0);
	CHECK_UINT(m.remote.port, 20000);
	CHECK_UINT(m.remote.rtcp_port, 20001);
	CHECK_UINT(m.codec, HAL_CODEC_PCMU);

	CASE("media addr: a remote port of 65535 empties the description, its "
	     "RTCP port would wrap to 0");
	snprintf(res.remote_addr, sizeof(res.remote_addr), "%s", T_PEER);
	res.remote_port = 65535;
	media_fill(&ch, true, 5, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.remote.port, 0);
	CHECK_UINT(m.codec, HAL_CODEC_NONE);

	CASE("media addr: 65534 is accepted and derives 65535 for RTCP");
	res.remote_port = 65534;
	media_fill(&ch, true, 5, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.remote.port, 65534);
	CHECK_UINT(m.remote.rtcp_port, 65535);
	CHECK_UINT(m.codec, HAL_CODEC_PCMU);
	res.remote_port = 20000;
}

/* ----------------------------------------------------- the HAL codec mask */

/*
 * One or two codec sections for every channel, in the order given. The builder
 * cases above drive the pure entry with a mask by hand; these drive a whole
 * leg, which is the crossing the builder cases cannot pin: a
 * leg_media_take() that passed ~0u would put a codec section on the wire
 * that the backend cannot run (call.h calls_hal_codecs_set).
 */
static void codec_sections_set(unsigned int first, unsigned int second)
{
	memset(cfg.codec, 0, sizeof(cfg.codec));
	cfg.codecs = second ? 2 : 1;
	cfg.codec[0].enabled = true;
	cfg.codec[0].tr104_id = first;
	cfg.codec[0].priority = 1;
	if (!second)
		return;

	cfg.codec[1].enabled = true;
	cfg.codec[1].tr104_id = second;
	cfg.codec[1].priority = 2;
}

static bool offer_has(const char *needle)
{
	struct req_info ri;

	return last_req(&ri) && has(ri.body, needle);
}

/* tr104_id 6 is Opus and 1 is PCMU (the uci codec section) */
#define TR104_OPUS	6
#define TR104_PCMU	1

static void test_codec_mask(void)
{
	unsigned int fallbacks, line;

	CASE("mask: a configured codec the HAL does not advertise stays off the offer");
	fx_reset();
	codec_sections_set(TR104_OPUS, TR104_PCMU);
	calls_reload(&cfg);
	calls_hal_codecs_set(~0u & ~(1u << HAL_CODEC_OPUS));
	CHECK(dial(0, 0, "1002"));
	CHECK(offer_has("m=audio 10000 RTP/AVP 0 101"));
	CHECK(!offer_has("opus"));

	CASE("mask: the same config on a HAL that advertises it offers the codec");
	fx_reset();
	codec_sections_set(TR104_OPUS, TR104_PCMU);
	calls_reload(&cfg);
	calls_hal_codecs_set(~0u);
	CHECK(dial(0, 0, "1002"));
	CHECK(offer_has("m=audio 10000 RTP/AVP 96 0 101"));
	CHECK(offer_has("a=rtpmap:96 opus/48000/2"));

	/*
	 * A line with no usable codec is refused, not quietly narrowed to the
	 * schema list: the fallback list is what a channel with no section of
	 * its own gets, and a section that names one unavailable codec is a
	 * configuration this board cannot serve (media_sdp_local_build).
	 */
	CASE("mask: a channel whose only section names an unadvertised codec dials nothing");
	fx_reset();
	codec_sections_set(TR104_OPUS, 0);
	calls_reload(&cfg);
	calls_hal_codecs_set(~0u & ~(1u << HAL_CODEC_OPUS));
	CHECK(!dial(0, 0, "1002"));
	CHECK_UINT(fk.sends, 0);

	CASE("mask: a channel with no section at all keeps the schema fallback");
	fx_reset();
	cfg.codecs = 0;
	calls_reload(&cfg);
	calls_hal_codecs_set(~0u);
	CHECK(dial(0, 0, "1002"));
	CHECK(offer_has("m=audio 10000 RTP/AVP 0 8 18 101"));

	CASE("mask: a reload does not clear the mask the next leg reads");
	fx_reset();
	codec_sections_set(TR104_OPUS, TR104_PCMU);
	calls_hal_codecs_set(~0u & ~(1u << HAL_CODEC_OPUS));
	calls_reload(&cfg);
	CHECK(dial(0, 0, "1002"));
	CHECK(!offer_has("opus"));

	CASE("mask: a mask pushed after the reload governs the next leg");
	calls_hal_codecs_set(~0u);
	CHECK(dial(1, 0, "1002"));
	CHECK(offer_has("a=rtpmap:96 opus/48000/2"));

	/*
	 * The answer side reads the same masked view. Every case above is
	 * offer-side, and sdp_answer_build() takes this leg's local view: a
	 * view built without the mask would answer a codec the board cannot
	 * run while every offer stayed correct. The peer names both
	 * telephone-event clocks, so each answer seats the one its codec rides
	 * and neither case drags the digit fallback in.
	 */
	CASE("mask: an inbound Opus offer is answered PCMU when the HAL lacks the codec");
	fx_reset();
	codec_sections_set(TR104_OPUS, TR104_PCMU);
	calls_reload(&cfg);
	calls_hal_codecs_set(~0u & ~(1u << HAL_CODEC_OPUS));
	rx(invite_text("ans-mask-1", NULL,
		       sdp_body(T_PEER, 20000, "96 0 101 102",
				"a=rtpmap:96 opus/48000/2\r\n"
				"a=rtpmap:102 telephone-event/48000\r\n")));
	call_ring(0, 0);
	CHECK(call_answer(0, 0));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(has(last_text(), "m=audio 10000 RTP/AVP 0 101"));
	/* the rtpmap form as well as the bare name: the Call-ID above must
	 * never carry the word this line refuses to find */
	CHECK(!has(last_text(), "opus"));
	CHECK(!has(last_text(), "opus/48000"));

	CASE("mask: the same offer is answered Opus when the HAL advertises it");
	fx_reset();
	codec_sections_set(TR104_OPUS, TR104_PCMU);
	calls_reload(&cfg);
	calls_hal_codecs_set(~0u);
	rx(invite_text("ans-mask-2", NULL,
		       sdp_body(T_PEER, 20000, "96 0 101 102",
				"a=rtpmap:96 opus/48000/2\r\n"
				"a=rtpmap:102 telephone-event/48000\r\n")));
	call_ring(0, 0);
	CHECK(call_answer(0, 0));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(has(last_text(), "m=audio 10000 RTP/AVP 96 102"));
	CHECK(has(last_text(), "a=rtpmap:96 opus/48000/2"));

	/*
	 * Once per exchange. The 2xx and the ACK program the same answer, so a
	 * report at the media fill counted every Opus call twice;
	 * media_program() reports only a description the stream did not
	 * already carry.
	 */
	CASE("mask: an Opus answer with no telephone-event/48000 reports the fallback once");
	fx_reset();
	codec_sections_set(TR104_OPUS, TR104_PCMU);
	calls_reload(&cfg);
	calls_hal_codecs_set(~0u);
	fallbacks = calls_opus_dtmf_fallbacks();
	warn_capture_begin();
	rx(invite_text("opus-1", NULL,
		       sdp_body(T_PEER, 20000, "96 101", "a=rtpmap:96 opus/48000/2\r\n")));
	call_ring(0, 0);
	call_answer(0, 0);
	rx(uas_in_dialog("ACK", 1, NULL, NULL));
	CHECK_UINT(med.config_calls, 1);
	CHECK_UINT(calls_opus_dtmf_fallbacks() - fallbacks, 1);
	CHECK_UINT(warn_capture_count("the peer named no telephone-event/48000"), 1);
	warn_capture_end();

	/*
	 * The SIP INFO channel offers no telephone-event of its own, so the
	 * answer seats none either and the same fallback runs: the report names
	 * the mode the digits actually take, which for this channel is INFO.
	 */
	CASE("mask: a SIP INFO channel keeps INFO and the report names it");
	fx_reset();
	codec_sections_set(TR104_OPUS, TR104_PCMU);
	for (line = 0; line < CFG_CHANNEL_MAX; line++)
		cfg.channel[line].dtmf_mode = CFG_DTMF_INFO;
	calls_reload(&cfg);
	calls_hal_codecs_set(~0u);
	fallbacks = calls_opus_dtmf_fallbacks();
	warn_capture_begin();
	rx(invite_text("opus-info", NULL,
		       sdp_body(T_PEER, 20000, "96 101", "a=rtpmap:96 opus/48000/2\r\n")));
	call_ring(0, 0);
	call_answer(0, 0);
	rx(uas_in_dialog("ACK", 1, NULL, NULL));
	CHECK_UINT(calls_opus_dtmf_fallbacks() - fallbacks, 1);
	CHECK_UINT(warn_capture_count("digits fall back to SIP INFO"), 1);
	warn_capture_end();
	for (line = 0; line < CFG_CHANNEL_MAX; line++)
		cfg.channel[line].dtmf_mode = CFG_DTMF_RFC2833;

	CASE("mask: an Opus exchange that keeps its event clock reports nothing");
	fx_reset();
	codec_sections_set(TR104_OPUS, TR104_PCMU);
	calls_reload(&cfg);
	calls_hal_codecs_set(~0u);
	fallbacks = calls_opus_dtmf_fallbacks();
	warn_capture_begin();
	rx(invite_text("opus-2", NULL,
		       sdp_body(T_PEER, 20000, "96 102",
				"a=rtpmap:96 opus/48000/2\r\n"
				"a=rtpmap:102 telephone-event/48000\r\n")));
	call_ring(0, 0);
	call_answer(0, 0);
	rx(uas_in_dialog("ACK", 1, NULL, NULL));
	CHECK_UINT(calls_opus_dtmf_fallbacks() - fallbacks, 0);
	CHECK_UINT(warn_capture_count("the peer named no telephone-event/48000"), 0);
	warn_capture_end();

	/*
	 * The narrowband refusal keeps its silence. A G.711 leg whose peer
	 * named no telephone-event falls back to inband exactly the same way,
	 * and it is the shipped behaviour of every board, so it is reported
	 * nowhere and counted nowhere.
	 */
	CASE("mask: a narrowband exchange with no telephone-event says nothing");
	fx_reset();
	cfg.codecs = 0;
	calls_reload(&cfg);
	calls_hal_codecs_set(~0u);
	fallbacks = calls_opus_dtmf_fallbacks();
	warn_capture_begin();
	rx(invite_text("pcmu-1", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	call_ring(0, 0);
	call_answer(0, 0);
	rx(uas_in_dialog("ACK", 1, NULL, NULL));
	CHECK_UINT(med.config_calls, 1);
	CHECK_UINT(calls_opus_dtmf_fallbacks() - fallbacks, 0);
	CHECK_UINT(warn_capture_count("digits fall back"), 0);
	warn_capture_end();

	/* the fixture is shared: every later case runs on the schema list */
	cfg_default();
	fx_reset();
}

/* -------------------------------------------------------- fail classes */

static void test_fail_map(void)
{
	CASE("fail: 486 is busy");
	CHECK_UINT(call_fail_map(486), CALL_FAIL_BUSY);

	CASE("fail: 600 is busy");
	CHECK_UINT(call_fail_map(600), CALL_FAIL_BUSY);

	CASE("fail: 603 is busy");
	CHECK_UINT(call_fail_map(603), CALL_FAIL_BUSY);

	CASE("fail: 404 is unobtainable");
	CHECK_UINT(call_fail_map(404), CALL_FAIL_UNOBTAINABLE);

	CASE("fail: 484 is unobtainable");
	CHECK_UINT(call_fail_map(484), CALL_FAIL_UNOBTAINABLE);

	CASE("fail: 604 is unobtainable");
	CHECK_UINT(call_fail_map(604), CALL_FAIL_UNOBTAINABLE);

	CASE("fail: 408 is congestion");
	CHECK_UINT(call_fail_map(408), CALL_FAIL_CONGESTION);

	CASE("fail: every 5xx is congestion");
	CHECK_UINT(call_fail_map(500), CALL_FAIL_CONGESTION);
	CHECK_UINT(call_fail_map(503), CALL_FAIL_CONGESTION);

	CASE("fail: an unmodelled code is congestion, never a free line");
	CHECK_UINT(call_fail_map(302), CALL_FAIL_CONGESTION);
	CHECK_UINT(call_fail_map(480), CALL_FAIL_CONGESTION);

	CASE("fail: every class has a name");
	CHECK(!strcmp(call_fail_name(CALL_FAIL_BUSY), "busy"));
	CHECK(!strcmp(call_fail_name(CALL_FAIL_CONGESTION), "congestion"));
	CHECK(!strcmp(call_fail_name(CALL_FAIL_UNOBTAINABLE), "unobtainable"));
	CHECK(!strcmp(call_fail_name(CALL_FAIL_QUIET), "quiet"));
}
/* ------------------------------------------------------------- scenarios */

static const char *cancel_text(const char *call_id)
{
	char *buf = text_slot();

	snprintf(buf, TEXT_MAX,
		 "CANCEL sip:%s@%s SIP/2.0\r\n"
		 "Via: SIP/2.0/UDP %s:%u;branch=z9hG4bK-in-%s\r\n"
		 "Max-Forwards: 70\r\n"
		 "From: \"Alice\" <sip:1002@%s>;tag=peer-%s\r\n"
		 "To: <sip:%s@%s>\r\n"
		 "Call-ID: %s\r\n"
		 "CSeq: 1 CANCEL\r\n"
		 "Content-Length: 0\r\n"
		 "\r\n",
		 T_USER, T_DOMAIN, T_PEER, T_PEER_PORT, call_id, T_DOMAIN, call_id, T_USER,
		 T_DOMAIN, call_id);

	return buf;
}

/* the To tag of a response we sent */
static bool resp_to_tag(unsigned int idx, char *out, size_t olen)
{
	static char copy[WIRE_MAX];
	struct sip_addr to;
	struct sip_msg m;
	size_t len;

	out[0] = 0;
	if (idx >= fk.sends || idx >= FAKE_SEND_MAX)
		return false;

	len = fk.send[idx].len;
	memcpy(copy, fk.send[idx].buf, len);
	if (msg_parse(&m, copy, len) != SIP_PARSE_OK || m.is_request)
		return false;
	if (!hdr_addr_get(&m, SIP_HDR_TO, &to))
		return false;

	return sip_str_copy(out, olen, to.tag);
}

static bool slot_is(unsigned int want)
{
	unsigned int slot = 99;

	return call_slot_free(0, &slot) && slot == want;
}

/*
 * The answer keeps telephone-event, so the negotiated DTMF mode is the
 * configured RFC 2833 one. An answer without it is a different case and has
 * its own test: it downgrades the stream, and with it the DTMF dispatch.
 */
static void uac_up(const char *se_extra)
{
	call_dial(0, 0, "1002", true);
	reply(100, NULL, NULL, NULL);
	reply(180, "peer-tag", NULL, NULL);
	reply(200, "peer-tag", se_extra, sdp_body(T_PEER, 20000, "0 101", NULL));
}

static void uas_up(const char *call_id)
{
	rx(invite_text(call_id, NULL, sdp_body(T_PEER, 20000, "0 8 101", NULL)));
	call_ring(0, 0);
	call_answer(0, 0);
	rx(uas_in_dialog("ACK", 1, NULL, NULL));
}

static void test_uac(void)
{
	struct req_info ri, first;
	unsigned int sends;

	fx_reset();

	CASE("uac: dial puts one INVITE on the wire");
	CHECK(call_dial(0, 0, "1002", true));
	CHECK_UINT(fk.sends, 1);
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.method, "INVITE"));

	CASE("uac: the request URI names the number in the account domain");
	CHECK(!strcmp(ri.ruri, "sip:1002@" T_DOMAIN));

	CASE("uac: the From carries the display name and a fresh tag");
	CHECK(has(ri.from, "Test Line"));
	CHECK(has(ri.from, "sip:1001@" T_DOMAIN));
	CHECK(ri.from_tag[0] != 0);

	CASE("uac: the To carries no tag");
	CHECK(ri.to_tag[0] == 0);

	CASE("uac: the Contact is our own address");
	CHECK(!strcmp(ri.contact, "sip:1001@" T_HOST ":5060"));

	CASE("uac: the served method set and the option tags are advertised");
	CHECK(has(send_text(0), "Allow: " UA_ALLOW_DEFAULT));
	CHECK(has(send_text(0), "Supported: timer"));

	CASE("uac: the request goes to the sticky registrar target");
	CHECK(!strcmp(fk.send[0].host, T_PEER));
	CHECK_UINT(fk.send[0].port, T_PEER_PORT);

	CASE("uac: the offer carries every configured codec and telephone-event");
	CHECK(has(ri.body, "m=audio 10000 RTP/AVP 0 8 18 101"));
	CHECK(has(ri.body, "a=rtpmap:101 telephone-event/8000"));
	CHECK(has(ri.body, "c=IN IP4 " T_HOST));
	CHECK(has(ri.body, "a=ptime:20"));

	CASE("uac: no Session-Expires is offered when none is configured");
	CHECK(!has(send_text(0), "Session-Expires"));

	CASE("uac: timer C bounds the setup");
	CHECK_UINT(timer_delay(SIP_TIMER_CALL_C), 180000);

	CASE("uac: a 100 alone reports nothing and sends nothing");
	reply(100, NULL, NULL, NULL);
	CHECK_UINT(ev.proceeding, 0);
	CHECK_UINT(fk.sends, 1);

	CASE("uac: an 18x reports proceeding without early media");
	reply(180, "peer-tag", NULL, NULL);
	CHECK_UINT(ev.proceeding, 1);
	CHECK(!ev.early_media);

	CASE("uac: a repeated 18x reports again");
	reply(180, "peer-tag", NULL, NULL);
	CHECK_UINT(ev.proceeding, 2);

	CASE("uac: a 2xx is acknowledged");
	reply(200, "peer-tag", NULL, sdp_body(T_PEER, 20000, "0", NULL));
	CHECK(find_send("ACK") == 1);

	CASE("uac: the ACK is in dialog and repeats the INVITE CSeq");
	CHECK(req_of("ACK", &ri));
	CHECK(req_info_at(0, &first));
	CHECK(!strcmp(ri.to_tag, "peer-tag"));
	CHECK(!strcmp(ri.call_id, first.call_id));
	CHECK_UINT(ri.cseq, 1);

	CASE("uac: the call is answered and nothing failed");
	CHECK_UINT(ev.answered, 1);
	CHECK_UINT(ev.failed, 0);

	CASE("uac: the stream is configured once and started once");
	CHECK_UINT(med.config_calls, 1);
	CHECK_UINT(med.start_calls, 1);

	CASE("uac: the stream carries the negotiation");
	CHECK_UINT(med.last.codec, HAL_CODEC_PCMU);
	CHECK_UINT(med.last.codec_pt, 0);
	CHECK_UINT(med.last.remote.port, 20000);
	CHECK_UINT(med.last.local.port, 10000);
	CHECK_UINT(med.last.direction, HAL_MEDIA_SENDRECV);
	CHECK_UINT(med.last_stream, 0);

	CASE("uac: timer C is cancelled by the answer");
	CHECK(!timer_armed(SIP_TIMER_CALL_C));

	CASE("uac: the slot is occupied while the call runs");
	CHECK(slot_is(1));

	CASE("uac: a hangup sends a BYE and stops the stream");
	CHECK(call_hangup(0, 0));
	CHECK(find_send("BYE") > 0);
	CHECK_UINT(med.stop_calls, 1);
	CHECK_UINT(ev.failed, 0);

	CASE("uac: the leg is released when its BYE transaction ends");
	reply(200, NULL, NULL, NULL);
	fake_advance(6000);
	CHECK(slot_is(0));

	CASE("uac: a line with an active account and a target is dial ready");
	fx_reset();
	CHECK(calls_line_dial_ready(0));
	CHECK(!calls_line_dial_ready(CFG_CHANNEL_MAX));

	CASE("uac: a dial with no registrar target fails congested without SIP");
	fx_reset();
	fk.dst_valid = false;
	CHECK(!calls_line_dial_ready(0));
	CHECK(!call_dial(0, 0, "1002", true));
	CHECK_UINT(fk.sends, 0);
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.cls, CALL_FAIL_CONGESTION);

	CASE("uac: a dial on an inactive account fails the same way");
	fx_reset();
	fk.active = false;
	CHECK(!calls_line_dial_ready(0));
	CHECK(!call_dial(0, 0, "1002", true));
	CHECK_UINT(fk.sends, 0);
	CHECK_UINT(ev.failed, 1);

	CASE("uac: a dial with no account at all fails the same way");
	fx_reset();
	fk.view_ok = false;
	CHECK(!calls_line_dial_ready(0));
	CHECK(!call_dial(0, 0, "1002", true));
	CHECK_UINT(ev.failed, 1);

	/* the IDLE SEIZE cell: a target alone is not enough, the account must
	 * hold a live registration too */
	CASE("uac: a line with a target and no live registration is not dial ready");
	fx_reset();
	fk.registered = false;
	CHECK(!calls_line_dial_ready(0));

	CASE("uac: a dial with no live registration fails congested without SIP");
	CHECK(!call_dial(0, 0, "1002", true));
	CHECK_UINT(fk.sends, 0);
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.cls, CALL_FAIL_CONGESTION);

	/* the registration lapses after the first call went up: the second
	 * dial is refused on its own slot and the held call is left alone */
	CASE("uac: a second dial with no live registration is refused and the held call stays");
	fx_reset();
	uac_up(NULL);
	CHECK(call_hold(0, 0));
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "0", "a=recvonly\r\n"));
	CHECK_UINT(ev.reinvite_done, 1);
	fk.registered = false;
	sends = fk.sends;
	CHECK(!call_dial(0, 1, "1003", true));
	CHECK_UINT(fk.sends, sends);
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.cls, CALL_FAIL_CONGESTION);
	CHECK(slot_is(1));

	CASE("uac: a re-INVITE and a BYE of the peer are served with no live registration");
	fx_reset();
	uac_up(NULL);
	fk.registered = false;
	rx(uac_in_dialog("INVITE", "peer-tag", 1, NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 200"));
	rx(uac_in_dialog("ACK", "peer-tag", 1, NULL, NULL));
	rx(uac_in_dialog("BYE", "peer-tag", 2, NULL, NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));

	CASE("uac: the dial ready answer asks no question of the wire");
	fx_reset();
	CHECK(calls_line_dial_ready(0));
	CHECK_UINT(fk.sends, 0);
	CHECK_UINT(ev.failed, 0);

	CASE("uac: an empty number is refused without an event");
	fx_reset();
	CHECK(!call_dial(0, 0, "", true));
	CHECK(!call_dial(0, 0, NULL, true));
	CHECK_UINT(ev.failed, 0);

	CASE("uac: a busy slot refuses a second dial");
	CHECK(call_dial(0, 0, "1002", true));
	CHECK(!call_dial(0, 0, "1003", true));
	CHECK_UINT(count_send("INVITE"), 1);

	/*
	 * RFC 4028 7.1 recommends an offer that leaves the refresher to the
	 * negotiation. A configured preference goes into the
	 * offer as it is: in the initial INVITE, the caller is the UAC.
	 */
	CASE("uac: a Session-Expires is offered when the line configures one");
	fx_reset();
	cfg.channel[0].session_max_expires = 1800;
	CHECK(call_dial(0, 0, "1002", true));
	CHECK(has(send_text(0), "Session-Expires: 1800\r\n"));

	CASE("uac: a configured uac refresher rides the offer");
	fx_reset();
	cfg.channel[0].session_refresher = CFG_REFRESH_UAC;
	CHECK(call_dial(0, 0, "1002", true));
	CHECK(has(send_text(0), "Session-Expires: 1800;refresher=uac\r\n"));

	CASE("uac: a configured uas refresher rides the offer");
	fx_reset();
	cfg.channel[0].session_refresher = CFG_REFRESH_UAS;
	CHECK(call_dial(0, 0, "1002", true));
	CHECK(has(send_text(0), "Session-Expires: 1800;refresher=uas\r\n"));
	cfg.channel[0].session_refresher = CFG_REFRESH_UNSPEC;
	cfg.channel[0].session_max_expires = 0;

	CASE("uac: a Min-SE floor is offered when the line configures one");
	fx_reset();
	cfg.channel[0].session_min_interval = 120;
	CHECK(call_dial(0, 0, "1002", true));
	CHECK(has(send_text(0), "Min-SE: 120"));
	cfg.channel[0].session_min_interval = 0;

	CASE("uac: a 2xx without an answer is acknowledged then cleared quietly");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	reply(100, NULL, NULL, NULL);
	reply(200, "peer-tag", NULL, NULL);
	CHECK(find_send("ACK") >= 0);
	CHECK(find_send("BYE") >= 0);
	CHECK_UINT(ev.answered, 0);
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.cls, CALL_FAIL_QUIET);

	CASE("uac: a 2xx with no common codec is acknowledged then cleared congested");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	reply(100, NULL, NULL, NULL);
	reply(200, "peer-tag", NULL,
	      sdp_body(T_PEER, 20000, "99", "a=rtpmap:99 SPEEX/8000\r\n"));
	CHECK(find_send("ACK") >= 0);
	CHECK(find_send("BYE") >= 0);
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.cls, CALL_FAIL_CONGESTION);

	CASE("uac: early media starts the stream in the negotiated direction");
	fx_reset();
	cfg.channel[0].early_media_enabled = true;
	CHECK(call_dial(0, 0, "1002", true));
	reply(100, NULL, NULL, NULL);
	reply(183, "peer-tag", NULL, sdp_body(T_PEER, 20000, "0", NULL));
	CHECK(ev.early_media);
	CHECK_UINT(med.config_calls, 1);
	CHECK_UINT(med.start_calls, 1);
	CHECK_UINT(med.last.direction, HAL_MEDIA_SENDRECV);

	CASE("uac: the 2xx answer governs the committed media");
	reply(200, "peer-tag", NULL, sdp_body(T_PEER, 21000, "8", NULL));
	CHECK_UINT(ev.answered, 1);
	CHECK_UINT(med.config_calls, 2);
	CHECK_UINT(med.last.codec, HAL_CODEC_PCMA);
	CHECK_UINT(med.last.remote.port, 21000);
	CHECK_UINT(med.last.direction, HAL_MEDIA_SENDRECV);

	/*
	 * The other three answers of RFC 3264 6.1. A gateway that plays an
	 * announcement and takes nothing answers sendonly, and this side then
	 * receives alone.
	 */
	CASE("uac: a sendonly 18x answer keeps the early stream receive only");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	reply(100, NULL, NULL, NULL);
	reply(183, "peer-tag", NULL,
	      sdp_body(T_PEER, 20000, "0", "a=sendonly\r\n"));
	CHECK(ev.early_media);
	CHECK_UINT(med.last.direction, HAL_MEDIA_RECVONLY);
	reply(200, "peer-tag", NULL,
	      sdp_body(T_PEER, 20000, "0", "a=sendrecv\r\n"));
	CHECK_UINT(ev.answered, 1);
	CHECK_UINT(med.last.direction, HAL_MEDIA_SENDRECV);

	CASE("uac: an inactive 18x answer programs the stream inactive");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	reply(100, NULL, NULL, NULL);
	reply(183, "peer-tag", NULL,
	      sdp_body(T_PEER, 20000, "0", "a=inactive\r\n"));
	CHECK(ev.early_media);
	CHECK_UINT(med.last.direction, HAL_MEDIA_INACTIVE);

	CASE("uac: a recvonly 18x answer runs the stream send-only");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	reply(100, NULL, NULL, NULL);
	reply(183, "peer-tag", NULL,
	      sdp_body(T_PEER, 20000, "0", "a=recvonly\r\n"));
	CHECK(ev.early_media);
	CHECK_UINT(med.last.direction, HAL_MEDIA_SENDONLY);
	cfg.channel[0].early_media_enabled = false;

	CASE("uac: an 18x answer is ignored while early media is off");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	reply(100, NULL, NULL, NULL);
	reply(183, "peer-tag", NULL, sdp_body(T_PEER, 20000, "0", NULL));
	CHECK(!ev.early_media);
	CHECK_UINT(med.config_calls, 0);

	CASE("uac: the answer still governs after an ignored 18x body");
	reply(200, "peer-tag", NULL, sdp_body(T_PEER, 20000, "0", NULL));
	CHECK_UINT(ev.answered, 1);
	CHECK_UINT(med.start_calls, 1);
}

/*
 * digit_esc_enabled as the URI escape policy: 0 raw, 1 '*' and '#', 2 '#'
 * alone. Mode 0 deliberately puts a raw '#' on the wire; that is the point
 * of the mode.
 */
static void test_dial_escape(void)
{
	struct req_info ri;

	CASE("escape 0: the dialled string rides the R-URI and the To raw");
	cfg_default();
	cfg.channel[0].digit_esc = 0;
	fx_reset();
	CHECK(call_dial(0, 0, "*84#", true));
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.ruri, "sip:*84#@" T_DOMAIN));
	CHECK(has(ri.to, "sip:*84#@" T_DOMAIN));

	CASE("escape 1: the star and the pound leave as %2A and %23");
	cfg_default();
	cfg.channel[0].digit_esc = 1;
	fx_reset();
	CHECK(call_dial(0, 0, "*84#", true));
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.ruri, "sip:%2A84%23@" T_DOMAIN));
	CHECK(has(ri.to, "sip:%2A84%23@" T_DOMAIN));

	CASE("escape 2: the pound leaves as %23 and the star stays");
	cfg_default();
	cfg.channel[0].digit_esc = 2;
	fx_reset();
	CHECK(call_dial(0, 0, "*84#", true));
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.ruri, "sip:*84%23@" T_DOMAIN));
	CHECK(has(ri.to, "sip:*84%23@" T_DOMAIN));

	CASE("the record keeps the raw number under the full escape");
	cfg_default();
	cfg.channel[0].digit_esc = 1;
	fx_reset();
	CHECK(call_dial(0, 0, "*84#", true));
	reply(100, NULL, NULL, NULL);
	reply(180, "peer-tag", NULL, NULL);
	reply(200, "peer-tag", NULL, sdp_body(T_PEER, 20000, "0 101", NULL));
	CHECK(call_hangup(0, 0));
	reply(200, NULL, NULL, NULL);
	fake_advance(6000);
	CHECK_UINT(ev.ended, 1);
	CHECK(!strcmp(ev.end.number, "*84#"));

	CASE("a blind transfer target is escaped in the Refer-To");
	cfg_default();
	cfg.channel[0].digit_esc = 1;
	fx_reset();
	uac_up(NULL);
	CHECK(call_xfer(0, 0, CALL_XFER_BLIND, "*84#"));
	CHECK(has(last_text(), "Refer-To: <sip:%2A84%23@" T_DOMAIN ">\r\n"));

	CASE("a full URI transfer target stays verbatim under the escape");
	cfg_default();
	cfg.channel[0].digit_esc = 1;
	fx_reset();
	uac_up(NULL);
	CHECK(call_xfer(0, 0, CALL_XFER_BLIND, "sip:bob@example.net"));
	CHECK(has(last_text(), "Refer-To: <sip:bob@example.net>\r\n"));

	CASE("a forward target is escaped in the 302 Contact");
	cfg_default();
	cfg.channel[0].digit_esc = 1;
	fx_reset();
	rx(invite_text("call-esc1", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(call_forward(0, 0, "*84#"));
	CHECK(has(last_text(), "Contact: <sip:%2A84%23@" T_DOMAIN ">"));

	cfg_default();
}

static void test_uac_auth(void)
{
	const char *chal = "WWW-Authenticate: Digest realm=\"" T_DOMAIN "\", nonce=\"" T_NONCE
			   "\"\r\n";
	const char *chal2 = "WWW-Authenticate: Digest realm=\"" T_DOMAIN
			    "\", nonce=\"a-second-nonce\"\r\n";
	const char *pchal = "Proxy-Authenticate: Digest realm=\"" T_DOMAIN "\", nonce=\"" T_NONCE
			    "\"\r\n";
	struct req_info ri, first;

	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	reply(100, NULL, NULL, NULL);

	CASE("auth: a 401 is acknowledged before anything else");
	reply(401, "peer-tag", chal, NULL);
	CHECK(find_send("ACK") == 1);

	CASE("auth: the INVITE is resent with credentials");
	CHECK_UINT(count_send("INVITE"), 2);
	CHECK(has(send_text(2), "Authorization: Digest"));
	CHECK(has(send_text(2), "nonce=\"" T_NONCE "\""));
	CHECK(has(send_text(2), "uri=\"sip:1002@" T_DOMAIN "\""));

	CASE("auth: the retry keeps the call and moves the CSeq and the branch");
	CHECK(last_req(&ri));
	CHECK(req_info_at(0, &first));
	CHECK_UINT(ri.cseq, 2);
	CHECK(!strcmp(ri.call_id, first.call_id));
	CHECK(!strcmp(ri.from_tag, first.from_tag));
	CHECK(strcmp(ri.branch, first.branch) != 0);

	CASE("auth: a second challenge of the same kind is not answered again");
	reply(100, NULL, NULL, NULL);
	reply(401, "peer-tag", chal2, NULL);
	CHECK_UINT(count_send("INVITE"), 2);
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.cls, CALL_FAIL_CONGESTION);

	CASE("auth: a proxy challenge is its own exchange");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	reply(100, NULL, NULL, NULL);
	reply(407, "peer-tag", pchal, NULL);
	CHECK_UINT(count_send("INVITE"), 2);
	CHECK(has(last_text(), "Proxy-Authorization: Digest"));

	CASE("auth: a challenge we cannot answer clears the call");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	reply(100, NULL, NULL, NULL);
	reply(401, "peer-tag", "WWW-Authenticate: Basic realm=\"x\"\r\n", NULL);
	CHECK_UINT(count_send("INVITE"), 1);
	CHECK_UINT(ev.failed, 1);
}

static void test_cancel(void)
{
	struct req_info ri, inv;

	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));

	CASE("cancel: a hangup before any provisional holds the CANCEL back");
	CHECK(call_hangup(0, 0));
	CHECK_UINT(fk.sends, 1);
	CHECK(find_send("CANCEL") < 0);

	CASE("cancel: the first provisional releases it");
	reply(100, NULL, NULL, NULL);
	CHECK(find_send("CANCEL") == 1);

	CASE("cancel: the CANCEL mirrors the INVITE (RFC 3261 9.1)");
	CHECK(req_of("CANCEL", &ri));
	CHECK(req_info_at(0, &inv));
	CHECK(!strcmp(ri.branch, inv.branch));
	CHECK(!strcmp(ri.call_id, inv.call_id));
	CHECK(!strcmp(ri.from, inv.from));
	CHECK(!strcmp(ri.to, inv.to));
	CHECK(!strcmp(ri.ruri, inv.ruri));
	CHECK_UINT(ri.cseq, inv.cseq);
	CHECK(ri.to_tag[0] == 0);

	CASE("cancel: the 487 ends the leg with no further event");
	reply_at(0, 487, "peer-tag", NULL, NULL);
	CHECK_UINT(ev.failed, 0);
	CHECK(slot_is(0));

	CASE("cancel: a 2xx that raced the CANCEL is acknowledged");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	reply(100, NULL, NULL, NULL);
	CHECK(call_hangup(0, 0));
	reply_at(0, 200, "peer-tag", NULL, sdp_body(T_PEER, 20000, "0", NULL));
	CHECK(find_send("ACK") > 0);

	CASE("cancel: and the call the 2xx created is ended at once");
	CHECK(find_send("BYE") > 0);

	CASE("cancel: the user hangup reports nothing to the FSM");
	CHECK_UINT(ev.failed, 0);
	CHECK_UINT(ev.answered, 0);

	CASE("cancel: an INVITE that dies before a provisional just releases");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	CHECK(call_hangup(0, 0));
	fake_advance(40000);
	CHECK(find_send("CANCEL") < 0);
	CHECK_UINT(ev.failed, 0);
	CHECK(slot_is(0));
}

static void test_glare(void)
{
	struct req_info ri;

	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	reply(100, NULL, NULL, NULL);

	CASE("glare: a 491 is acknowledged and arms the retry timer");
	reply(491, "peer-tag", NULL, NULL);
	CHECK(find_send("ACK") == 1);
	CHECK(timer_armed(SIP_TIMER_GLARE));

	CASE("glare: the Call-ID owner waits between 2.1 and 4.0 seconds");
	CHECK(timer_delay(SIP_TIMER_GLARE) >= 2100);
	CHECK(timer_delay(SIP_TIMER_GLARE) <= 4000);

	CASE("glare: the retry re-offers on a new CSeq");
	fake_advance(4000);
	CHECK_UINT(count_send("INVITE"), 2);
	CHECK(last_req(&ri));
	CHECK_UINT(ri.cseq, 2);
	CHECK(has(ri.body, "m=audio 10000 RTP/AVP 0 8 18 101"));

	CASE("glare: the retry is bounded and the call then clears");
	reply(100, NULL, NULL, NULL);
	reply(491, "peer-tag", NULL, NULL);
	fake_advance(4000);
	CHECK(req_of("INVITE", &ri));
	CHECK_UINT(ri.cseq, 3);
	reply(100, NULL, NULL, NULL);
	reply(491, "peer-tag", NULL, NULL);
	fake_advance(4000);
	CHECK(req_of("INVITE", &ri));
	CHECK_UINT(ri.cseq, 3);
	CHECK_UINT(ev.failed, 1);
}

/*
 * The fax tone helpers of test_fax_exit() below, because the backoff has to
 * hold the two deferrals a carried fax makes and the arm is the same one.
 */
static void exit_tone_arm(uint16_t code, uint8_t dir);
static enum call_fax_tone exit_tone_end(uint16_t code, uint8_t dir, uint16_t ms);
static uint32_t exit_handle(void);

/*
 * The backoff of a re-INVITE 491 (RFC 3261 14.1). No INVITE of ours is in
 * progress while the retry timer runs, so RFC 3261 14.2 asks for no 491 from
 * this side. The leg goes back to active and answers the peer, whose own
 * retry arrives inside our window. An outbound leg owns the Call-ID, so its
 * window opens at 2.1 s, and a field switch retries after 1.9 s.
 *
 * The re-INVITE of the peer is the only answer that the backoff changes.
 * Every other gate on "no exchange of my own" reads leg_free_is(), which is
 * false during the backoff. Those gates refuse as they do while our operation
 * runs: a REFER in the backoff is answered 491, and a second operation is
 * refused.
 */
static void test_glare_wait(void)
{
	unsigned int before, configs;
	struct req_info ri;
	struct slot_view sv;

	fx_reset();
	uac_up(NULL);
	CHECK(call_hold(0, 0));
	reply(491, NULL, NULL, NULL);
	CHECK(timer_armed(SIP_TIMER_GLARE));

	CASE("glare wait: a peer re-INVITE during our backoff is answered 200, not 491");
	rx(uac_in_dialog("INVITE", "peer-tag", 1, NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 200"));

	/* the hold offer was rolled back with the 491, so the answer reads the
	 * direction of the call and not the one our own offer asked for */
	CASE("glare wait: that answer is built from the state before our own operation");
	CHECK(has(last_text(), "a=sendrecv"));
	CHECK(!has(last_text(), "a=sendonly"));

	CASE("glare wait: the peer exchange commits and our operation is still waiting");
	rx(uac_in_dialog("ACK", "peer-tag", 1, NULL, NULL));
	CHECK(timer_armed(SIP_TIMER_GLARE));
	CHECK_UINT(ev.reinvite_done, 0);
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.state, "active"));

	CASE("glare wait: the timer then sends the retry with the operation it held");
	fake_advance(4000);
	CHECK(req_of("INVITE", &ri));
	CHECK_UINT(ri.cseq, 3);
	CHECK(has(ri.body, "a=sendonly"));

	CASE("glare wait: a peer exchange unacknowledged at expiry re-arms the wait");
	fx_reset();
	uac_up(NULL);
	CHECK(call_hold(0, 0));
	reply(491, NULL, NULL, NULL);
	rx(uac_in_dialog("INVITE", "peer-tag", 1, NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 200"));
	before = count_send("INVITE");
	fake_advance(4000);
	CHECK_UINT(count_send("INVITE"), before);
	CHECK(timer_armed(SIP_TIMER_GLARE));

	CASE("glare wait: and the retry leaves once that exchange completes");
	rx(uac_in_dialog("ACK", "peer-tag", 1, NULL, NULL));
	fake_advance(4000);
	CHECK(count_send("INVITE") > before);
	CHECK(req_of("INVITE", &ri));
	CHECK_UINT(ri.cseq, 3);
	CHECK(has(ri.body, "a=sendonly"));

	CASE("glare wait: a hangup during the backoff cancels the timer and sends nothing");
	fx_reset();
	uac_up(NULL);
	CHECK(call_hold(0, 0));
	reply(491, NULL, NULL, NULL);
	CHECK(call_hangup(0, 0));
	CHECK(!timer_armed(SIP_TIMER_GLARE));
	before = count_send("INVITE");
	fake_advance(4000);
	CHECK_UINT(count_send("INVITE"), before);

	CASE("glare wait: a second operation during the backoff is refused, and the first still completes");
	fx_reset();
	uac_up(NULL);
	CHECK(call_hold(0, 0));
	reply(491, NULL, NULL, NULL);
	CHECK(!call_resume(0, 0));
	CHECK_UINT(ev.reinvite_done, 0);
	fake_advance(4000);
	CHECK(last_req(&ri));
	CHECK(has(ri.body, "a=sendonly"));
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "0", "a=recvonly\r\n"));
	CHECK_UINT(ev.reinvite_done, 1);
	CHECK(ev.reinvite_ok);
	CHECK_UINT(med.last.direction, HAL_MEDIA_SENDONLY);

	CASE("glare wait: a session refresh that comes due in the backoff arms again and sends nothing");
	fx_reset();
	cfg.channel[0].session_max_expires = 1800;
	uac_up("Session-Expires: 1800;refresher=uac\r\n");
	CHECK(call_hold(0, 0));
	reply(491, NULL, NULL, NULL);
	before = count_send("INVITE");
	timer_fire_early(SIP_TIMER_CALL_SESSION, 0);
	CHECK_UINT(count_send("INVITE"), before);
	CHECK_UINT(timer_delay(SIP_TIMER_CALL_SESSION), 900000);
	CHECK(timer_armed(SIP_TIMER_GLARE));
	cfg.channel[0].session_max_expires = 0;

	CASE("glare wait: and the held hold still goes out at its own timer");
	fake_advance(4000);
	CHECK(req_of("INVITE", &ri));
	CHECK(has(ri.body, "a=sendonly"));

	CASE("glare wait: a fax tone in the backoff starts no upspeed, and the hold still completes");
	fx_reset();
	uac_up(NULL);
	CHECK(call_hold(0, 0));
	reply(491, NULL, NULL, NULL);
	before = count_send("INVITE");
	CHECK(!call_fax_start(0, 0));
	CHECK_UINT(med.fax_calls, 0);
	CHECK_UINT(count_send("INVITE"), before);
	fake_advance(4000);
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "0", "a=recvonly\r\n"));
	CHECK_UINT(ev.reinvite_done, 1);
	CHECK(ev.reinvite_ok);

	CASE("glare wait: a REFER in the backoff is answered 491, as before");
	fx_reset();
	uac_up(NULL);
	CHECK(call_hold(0, 0));
	reply(491, NULL, NULL, NULL);
	rx(uac_in_dialog("REFER", "peer-tag", 1,
			 "Refer-To: <sip:1003@" T_DOMAIN ">\r\n", NULL));
	CHECK(has(last_text(), "SIP/2.0 491"));
	CHECK(find_send("INVITE sip:1003") < 0);

	CASE("glare wait: a T.38 start in the backoff answers busy");
	fx_reset();
	uac_up(NULL);
	CHECK(call_hold(0, 0));
	reply(491, NULL, NULL, NULL);
	CHECK(call_t38_start(0, 0) == CALL_T38_BUSY);

	CASE("glare wait: a retry that cannot start reports the held operation failed, "
	     "so the machine leaves PENDING");
	fx_reset();
	uac_up(NULL);
	CHECK(call_hold(0, 0));
	reply(491, NULL, NULL, NULL);
	fk.send_fail = true;
	fake_advance(4000);
	fk.send_fail = false;
	CHECK_UINT(ev.reinvite_done, 1);
	CHECK(!ev.reinvite_ok);
	CHECK_UINT(ev.failed, 0);
	CHECK(!timer_armed(SIP_TIMER_GLARE));

	CASE("glare wait: a consult transfer during the backoff silences the leg and sends none");
	fx_reset();
	CHECK(dial(0, 0, "1002"));
	reply(100, NULL, NULL, NULL);
	reply(200, "peer-a", NULL, sdp_body(T_PEER, 20000, "0 101", NULL));
	CHECK(call_hold(0, 0));
	reply(491, NULL, NULL, NULL);
	CHECK(dial(0, 1, "1003"));
	reply(100, NULL, NULL, NULL);
	reply(200, "peer-b", "Contact: <sip:1003@203.0.113.9:5062>\r\n",
	      sdp_body(T_PEER, 20002, "0 101", NULL));
	CHECK(call_xfer_consult(0, 0, 1));
	reply(202, NULL, NULL, NULL);
	CHECK(!timer_armed(SIP_TIMER_GLARE));
	before = count_send("INVITE");
	fake_advance(4000);
	CHECK_UINT(count_send("INVITE"), before);

	CASE("glare wait: the two retry budget still ends the second 491");
	fx_reset();
	uac_up(NULL);
	CHECK(call_hold(0, 0));
	reply(491, NULL, NULL, NULL);
	fake_advance(4000);
	reply(491, NULL, NULL, NULL);
	fake_advance(4000);
	CHECK(req_of("INVITE", &ri));
	CHECK_UINT(ri.cseq, 4);
	reply(491, NULL, NULL, NULL);
	CHECK(!timer_armed(SIP_TIMER_GLARE));
	CHECK_UINT(ev.reinvite_done, 1);
	CHECK(!ev.reinvite_ok);
	CHECK_UINT(ev.failed, 0);

	CASE("glare wait: a peer offerless re-INVITE during the backoff still puts our offer in the 200");
	fx_reset();
	uac_up(NULL);
	CHECK(call_hold(0, 0));
	reply(491, NULL, NULL, NULL);
	rx(uac_in_dialog("INVITE", "peer-tag", 1, NULL, NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(has(last_text(), "m=audio 10000 RTP/AVP 0 8 18 101"));
	CHECK(!has(last_text(), "a=sendonly"));
	rx(uac_in_dialog("ACK", "peer-tag", 1, NULL, sdp_body(T_PEER, 22000, "8", NULL)));
	CHECK_UINT(med.last.remote.port, 22000);
	CHECK_UINT(med.last.codec, HAL_CODEC_PCMA);

	/*
	 * A refused command must not spend the budget of the exchange that is
	 * waiting: the clear belongs behind the start. Without that, the third
	 * 491 of this hold arms a fourth attempt.
	 */
	CASE("glare wait: an operation refused in the backoff leaves the budget alone");
	fx_reset();
	uac_up(NULL);
	CHECK(call_hold(0, 0));
	reply(491, NULL, NULL, NULL);
	CHECK(!call_resume(0, 0));
	fake_advance(4000);
	CHECK(req_of("INVITE", &ri));
	CHECK_UINT(ri.cseq, 3);
	reply(491, NULL, NULL, NULL);
	fake_advance(4000);
	CHECK(req_of("INVITE", &ri));
	CHECK_UINT(ri.cseq, 4);
	reply(491, NULL, NULL, NULL);
	CHECK(!timer_armed(SIP_TIMER_GLARE));
	fake_advance(4000);
	CHECK(req_of("INVITE", &ri));
	CHECK_UINT(ri.cseq, 4);
	CHECK_UINT(ev.reinvite_done, 1);
	CHECK(!ev.reinvite_ok);

	/*
	 * The hold a transferee's child re-offers rides CALL_OP_XFER_HOLD. Its
	 * retry has to ask for the hold again: the 491 put the leg back to the
	 * state of before the offer, and an offer without the hold resumes a
	 * call the machine still believes is held.
	 */
	CASE("glare wait: a 491 to a transferee child's re-offered hold retries as a hold");
	fx_reset();
	uac_up(NULL);
	CHECK(call_hold(0, 0));
	reply(200, "peer-tag", NULL, sdp_body(T_PEER, 20000, "0 101", NULL));
	rx(uac_in_dialog("REFER", "peer-tag", 3,
			 "Refer-To: <sip:1003@" T_DOMAIN ">\r\n", NULL));
	/* the referral placed the child INVITE, which is the last thing out */
	before = fk.sends - 1;
	reply_at(before, 200, "target-tag", NULL, sdp_body(T_PEER, 20002, "0 101", NULL));
	CHECK(req_of("INVITE", &ri));
	CHECK_UINT(ri.cseq, 2);
	CHECK(has(ri.body, "a=sendonly"));
	reply(491, NULL, NULL, NULL);
	CHECK(timer_armed(SIP_TIMER_GLARE));
	/* the served referral still retransmits its NOTIFY here, so the
	 * retry is named by method and CSeq, not as the last thing out */
	fake_advance(4000);
	CHECK(req_of("INVITE", &ri));
	CHECK_UINT(ri.cseq, 3);
	CHECK(has(ri.body, "a=sendonly"));

	CASE("glare wait: a fax kind in the backoff waits and reaches the stream at the retry");
	fx_reset();
	uac_up(NULL);
	call_fax_hint(0, 0);
	exit_tone_arm(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE);
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_MODEM);
	CHECK(call_hold(0, 0));
	reply(491, NULL, NULL, NULL);
	configs = med.config_calls;
	CHECK(call_fax_kind_set(0, 0, HAL_PASSTHROUGH_FAX));
	CHECK_UINT(med.config_calls, configs);
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_MODEM);
	fake_advance(4000);
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "0", "a=recvonly\r\n"));
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_FAX);

	CASE("glare wait: a return to voice in the backoff waits and completes at the retry");
	fx_reset();
	uac_up(NULL);
	call_fax_hint(0, 0);
	exit_tone_arm(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE);
	CHECK(call_hold(0, 0));
	reply(491, NULL, NULL, NULL);
	configs = med.config_calls;
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE, 900),
		   CALL_FAX_TONE_ENDED);
	CHECK(call_fax_exit(exit_handle()));
	CHECK_UINT(med.config_calls, configs);
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_MODEM);
	fake_advance(4000);
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "0", "a=recvonly\r\n"));
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_OFF);
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!sv.fax[0]);

	/*
	 * The upspeed clamped the leg's codec list to one G.711 entry, and a
	 * 491 does not put that list back (the local profile stays). The answer
	 * the peer gets during the backoff is therefore the clamped one.
	 */
	CASE("glare wait: a peer re-INVITE in a fax backoff is answered from the clamped list");
	fx_reset();
	uac_up(NULL);
	CHECK(call_fax_start(0, 0));
	reply(491, NULL, NULL, NULL);
	CHECK(timer_armed(SIP_TIMER_GLARE));
	rx(uac_in_dialog("INVITE", "peer-tag", 1, NULL,
			 sdp_body(T_PEER, 20000, "0 8 18 101", NULL)));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(has(last_text(), "m=audio 10000 RTP/AVP 0 101"));
	CHECK(!has(last_text(), "a=rtpmap:18 G729/8000"));
}

static void test_min_se(void)
{
	fx_reset();
	cfg.channel[0].session_max_expires = 600;

	CASE("422: the INVITE asks for the configured session length");
	CHECK(call_dial(0, 0, "1002", true));
	CHECK(has(send_text(0), "Session-Expires: 600"));
	reply(100, NULL, NULL, NULL);

	CASE("422: a Min-SE above our request raises it");
	reply(422, "peer-tag", "Min-SE: 900\r\n", NULL);
	CHECK_UINT(count_send("INVITE"), 2);
	CHECK(has(last_text(), "Session-Expires: 900"));
	CHECK(has(last_text(), "Min-SE: 900"));

	CASE("422: the raised request is acknowledged first");
	CHECK(find_send("ACK") == 1);

	/*
	 * RFC 4028 6 makes the retry unconditional: a Min-SE at or below what
	 * we offered still gets one, with the interval unchanged. The live
	 * uas_session_timer_422 run met exactly this peer, and the e2e scenario
	 * of that name sends it.
	 */
	CASE("422: a Min-SE below our offer still gets one retry");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	reply(100, NULL, NULL, NULL);
	reply(422, "peer-tag", "Min-SE: 120\r\n", NULL);
	CHECK_UINT(count_send("INVITE"), 2);
	CHECK_UINT(ev.failed, 0);

	CASE("422: the retry keeps the interval and echoes the peer Min-SE");
	CHECK(has(last_text(), "Session-Expires: 600"));
	CHECK(has(last_text(), "Min-SE: 120"));

	CASE("422: e2e uas_session_timer_422, 1800 refused with Min-SE 900");
	fx_reset();
	cfg.channel[0].session_max_expires = 1800;
	CHECK(call_dial(0, 0, "1002", true));
	CHECK(has(send_text(0), "Session-Expires: 1800"));
	reply(100, NULL, NULL, NULL);
	reply(422, "peer-tag", "Min-SE: 900\r\n", NULL);
	CHECK_UINT(count_send("INVITE"), 2);
	CHECK(has(last_text(), "Session-Expires: 1800"));
	CHECK(has(last_text(), "Min-SE: 900"));
	CHECK_UINT(ev.failed, 0);

	CASE("422: the e2e retry completes the call");
	reply(100, NULL, NULL, NULL);
	reply(200, "peer-tag", "Session-Expires: 1800;refresher=uac\r\n",
	      sdp_body(T_PEER, 20000, "0 101", NULL));
	CHECK_UINT(ev.answered, 1);
	CHECK_UINT(ev.failed, 0);
	cfg.channel[0].session_max_expires = 600;

	CASE("422: a 422 with no Min-SE at all is retried once, then clears");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	reply(100, NULL, NULL, NULL);
	reply(422, "peer-tag", NULL, NULL);
	CHECK_UINT(count_send("INVITE"), 2);
	CHECK(has(last_text(), "Session-Expires: 600"));
	reply(100, NULL, NULL, NULL);
	reply(422, "peer-tag", NULL, NULL);
	CHECK_UINT(count_send("INVITE"), 2);
	CHECK_UINT(ev.failed, 1);

	CASE("422: the retry happens once");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	reply(100, NULL, NULL, NULL);
	reply(422, "peer-tag", "Min-SE: 900\r\n", NULL);
	reply(100, NULL, NULL, NULL);
	reply(422, "peer-tag", "Min-SE: 1800\r\n", NULL);
	CHECK_UINT(count_send("INVITE"), 2);
	CHECK_UINT(ev.failed, 1);

	CASE("422: the second one clears with the congestion class");
	CHECK_UINT(ev.cls, CALL_FAIL_CONGESTION);
	CHECK(slot_is(0));
	cfg.channel[0].session_max_expires = 0;
}

static void test_session(void)
{
	struct req_info ri;

	fx_reset();
	cfg.channel[0].session_max_expires = 1800;
	uac_up("Session-Expires: 1800;refresher=uac\r\n");

	CASE("session: the refresher arms the refresh at half the interval");
	CHECK_UINT(ev.answered, 1);
	CHECK_UINT(timer_delay(SIP_TIMER_CALL_SESSION), 900000);
	CHECK(!timer_armed(SIP_TIMER_CALL_WATCHDOG));

	CASE("session: the refresh is a re-INVITE repeating the committed offer");
	fake_advance(900000);
	CHECK_UINT(count_send("INVITE"), 2);
	CHECK(last_req(&ri));
	CHECK_UINT(ri.cseq, 2);
	CHECK(has(ri.body, "m=audio 10000 RTP/AVP 0 8 18 101"));
	CHECK(!strcmp(ri.contact, "sip:1001@" T_HOST ":5060"));

	CASE("session: the refresh is not a feature operation of the FSM");
	reply(200, NULL, "Session-Expires: 1800;refresher=uac\r\n",
	      sdp_body(T_PEER, 20000, "0 101", NULL));
	CHECK_UINT(ev.reinvite_done, 0);
	CHECK_UINT(ev.failed, 0);

	CASE("session: an accepted refresh re-arms the timer and is acknowledged");
	CHECK_UINT(timer_delay(SIP_TIMER_CALL_SESSION), 900000);
	CHECK_UINT(count_send("ACK"), 2);

	CASE("session: the media did not move under the refresh");
	CHECK_UINT(med.config_calls, 1);
	CHECK_UINT(med.start_calls, 1);

	CASE("session: a peer refresher arms the watchdog instead");
	fx_reset();
	cfg.channel[0].session_max_expires = 1800;
	uac_up("Session-Expires: 1800;refresher=uas\r\n");
	CHECK_UINT(timer_delay(SIP_TIMER_CALL_WATCHDOG), 1768000);
	CHECK(!timer_armed(SIP_TIMER_CALL_SESSION));

	CASE("session: a peer that stops refreshing is cleared quietly");
	fake_advance(1768000);
	CHECK_UINT(count_send("BYE"), 1);
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.cls, CALL_FAIL_QUIET);
	CHECK_UINT(med.stop_calls, 1);

	CASE("session: a refresh that fails clears the call quietly");
	fx_reset();
	cfg.channel[0].session_max_expires = 1800;
	uac_up("Session-Expires: 1800;refresher=uac\r\n");
	fake_advance(900000);
	reply(408, NULL, NULL, NULL);
	CHECK(find_send("BYE") > 0);
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.cls, CALL_FAIL_QUIET);
	CHECK_UINT(ev.reinvite_done, 0);

	CASE("session: no Session-Expires in the 2xx means no session timer");
	fx_reset();
	cfg.channel[0].session_max_expires = 1800;
	uac_up(NULL);
	CHECK(!timer_armed(SIP_TIMER_CALL_SESSION));
	CHECK(!timer_armed(SIP_TIMER_CALL_WATCHDOG));

	CASE("session: a peer interval is honoured even with none configured");
	fx_reset();
	cfg.channel[0].session_max_expires = 0;
	uac_up("Session-Expires: 1800;refresher=uas\r\n");
	CHECK_UINT(timer_delay(SIP_TIMER_CALL_WATCHDOG), 1768000);

	CASE("session: an absurd interval is clamped before it reaches a timer");
	fx_reset();
	cfg.channel[0].session_max_expires = 1800;
	uac_up("Session-Expires: 4294968;refresher=uac\r\n");
	CHECK_UINT(timer_delay(SIP_TIMER_CALL_SESSION), 43200000);
	CHECK_UINT(ev.failed, 0);

	CASE("session: the configured refresher decides when the peer is silent");
	fx_reset();
	cfg.channel[0].session_max_expires = 1800;
	cfg.channel[0].session_refresher = CFG_REFRESH_UAS;
	uac_up("Session-Expires: 1800\r\n");
	CHECK(timer_armed(SIP_TIMER_CALL_WATCHDOG));
	cfg.channel[0].session_refresher = CFG_REFRESH_UNSPEC;

	/*
	 * A refresh keeps the hold silence. The refresh repeats the committed
	 * offer and reaches the same media choke point every other exchange
	 * does, so a description built without the leg's hold state would open
	 * the uplink for the rest of the call while the wire still reads
	 * sendonly. The counter is what carries the "every configuration" part:
	 * med.last would also pass on a leg that went live for one exchange and
	 * silent again.
	 */
	CASE("session: a refresh taken while held never opens the uplink");
	fx_reset();
	cfg.channel[0].session_max_expires = 1800;
	uac_up("Session-Expires: 1800;refresher=uac\r\n");
	CHECK(call_hold(0, 0));
	reply(200, NULL, "Session-Expires: 1800;refresher=uac\r\n",
	      sdp_body(T_PEER, 20000, "0", "a=recvonly\r\n"));
	CHECK_UINT(ev.reinvite_done, 1);
	CHECK(med.last.tx_silence);
	med.config_live = 0;
	fake_advance(900000);
	CHECK(req_of("INVITE", &ri));
	CHECK(has(ri.body, "a=sendonly"));
	reply(200, NULL, "Session-Expires: 1800;refresher=uac\r\n",
	      sdp_body(T_PEER, 20000, "0", "a=recvonly\r\n"));
	CHECK_UINT(med.config_live, 0);
	CHECK(med.last.tx_silence);
	CHECK_UINT(med.last.direction, HAL_MEDIA_SENDONLY);

	/*
	 * The 491 budget belongs to one exchange (RFC 3261 14.1). A hold that
	 * spent both retries hours before must not leave the first 491 of a
	 * refresh with none, because a refresh that fails takes the call down
	 * with it (RFC 4028 10).
	 */
	CASE("session: a refresh resets the 491 budget an earlier hold spent");
	fx_reset();
	cfg.channel[0].session_max_expires = 1800;
	uac_up("Session-Expires: 1800;refresher=uac\r\n");
	CHECK(call_hold(0, 0));
	reply(491, NULL, NULL, NULL);
	fake_advance(4000);
	reply(491, NULL, NULL, NULL);
	fake_advance(4000);
	reply(200, NULL, "Session-Expires: 1800;refresher=uac\r\n",
	      sdp_body(T_PEER, 20000, "0", "a=recvonly\r\n"));
	CHECK_UINT(ev.reinvite_done, 1);
	CHECK(ev.reinvite_ok);
	fake_advance(900000);
	CHECK(req_of("INVITE", &ri));
	CHECK_UINT(ri.cseq, 5);
	reply(491, NULL, NULL, NULL);
	CHECK(timer_armed(SIP_TIMER_GLARE));
	CHECK_UINT(ev.failed, 0);
	CHECK(find_send("BYE") < 0);

	/*
	 * And the budget the refresh opens is spent by its own retries alone: a
	 * reset at each retry would give one refresh an endless one.
	 */
	CASE("session: and the refresh keeps the budget of its own exchange");
	fx_reset();
	cfg.channel[0].session_max_expires = 1800;
	uac_up("Session-Expires: 1800;refresher=uac\r\n");
	fake_advance(900000);
	CHECK(req_of("INVITE", &ri));
	CHECK_UINT(ri.cseq, 2);
	reply(491, NULL, NULL, NULL);
	fake_advance(4000);
	CHECK(req_of("INVITE", &ri));
	CHECK_UINT(ri.cseq, 3);
	reply(491, NULL, NULL, NULL);
	CHECK(timer_armed(SIP_TIMER_GLARE));
	fake_advance(4000);
	CHECK(req_of("INVITE", &ri));
	CHECK_UINT(ri.cseq, 4);
	reply(491, NULL, NULL, NULL);
	CHECK(!timer_armed(SIP_TIMER_GLARE));

	CASE("session: a third 491 on one refresh still clears the call quietly");
	CHECK(find_send("BYE") > 0);
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.cls, CALL_FAIL_QUIET);
	CHECK_UINT(ev.reinvite_done, 0);

	cfg.channel[0].session_max_expires = 0;
}

/*
 * RFC 4028 7.2 and 7.4: the refresher parameter names a side of the
 * transaction that carries it, not of the dialog. The two differ for every
 * request of the called side, so each case takes a leg through an exchange
 * in the direction that tells the two readings apart.
 *
 * The reference is a 120 s session through a switch that refreshes as the
 * UAS. The called line refreshes with refresher=uac and keeps the role. The
 * calling line answers the refresh of the switch with refresher=uac and keeps
 * its watchdog.
 */
static void test_session_refresher(void)
{
	struct req_info ri;

	fx_reset();
	cfg.channel[0].session_max_expires = 120;
	cfg.channel[0].session_refresher = CFG_REFRESH_UAS;
	uas_up("call-sr1");

	CASE("refresher: a called line configured uas answers as the refresher");
	CHECK(has(send_text((unsigned int)find_send("SIP/2.0 200")),
		  "Session-Expires: 120;refresher=uas\r\n"));
	CHECK_UINT(timer_delay(SIP_TIMER_CALL_SESSION), 60000);
	CHECK(!timer_armed(SIP_TIMER_CALL_WATCHDOG));

	CASE("refresher: its refresh names itself uac, the side that sends it");
	fake_advance(60000);
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.method, "INVITE"));
	CHECK(has(last_text(), "Session-Expires: 120;refresher=uac\r\n"));

	CASE("refresher: a 2xx that names the uac keeps the refresh with the called line");
	reply(200, NULL, "Require: timer\r\nSession-Expires: 120;refresher=uac\r\n",
	      sdp_body(T_PEER, 20000, "0 101", NULL));
	CHECK_UINT(timer_delay(SIP_TIMER_CALL_SESSION), 60000);
	CHECK(!timer_armed(SIP_TIMER_CALL_WATCHDOG));
	fake_advance(60000);
	CHECK_UINT(count_send("INVITE"), 2);
	CHECK_UINT(ev.failed, 0);

	fx_reset();
	uac_up("Session-Expires: 120;refresher=uas\r\n");

	CASE("refresher: a calling line whose peer refreshes answers that refresh uac");
	CHECK_UINT(timer_delay(SIP_TIMER_CALL_WATCHDOG), 88000);
	rx(uac_in_dialog("INVITE", "peer-tag", 2,
			 "Supported: timer\r\nSession-Expires: 120;refresher=uac\r\n",
			 sdp_body(T_PEER, 20000, "0 101", NULL)));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(has(last_text(), "Session-Expires: 120;refresher=uac\r\n"));

	CASE("refresher: and it keeps its watchdog instead of taking the refresh");
	CHECK_UINT(timer_delay(SIP_TIMER_CALL_WATCHDOG), 88000);
	CHECK(!timer_armed(SIP_TIMER_CALL_SESSION));
	rx(uac_in_dialog("ACK", "peer-tag", 2, NULL, NULL));
	CHECK(!timer_armed(SIP_TIMER_CALL_SESSION));

	CASE("refresher: an unspecified called line takes the refresh itself");
	fx_reset();
	cfg.channel[0].session_refresher = CFG_REFRESH_UNSPEC;
	uas_up("call-sr3");
	CHECK(has(send_text((unsigned int)find_send("SIP/2.0 200")),
		  "Session-Expires: 120;refresher=uas\r\n"));
	CHECK_UINT(timer_delay(SIP_TIMER_CALL_SESSION), 60000);

	CASE("refresher: an unspecified calling line refreshes when the 2xx names nobody");
	fx_reset();
	uac_up("Session-Expires: 120\r\n");
	CHECK_UINT(timer_delay(SIP_TIMER_CALL_SESSION), 60000);
	CHECK(!timer_armed(SIP_TIMER_CALL_WATCHDOG));

	/*
	 * A re-INVITE can leave the refresher open (RFC 4028 7.4). The line
	 * that answers it decides, and a configured preference names a side of
	 * the call, so it gives the same answer in either direction.
	 */
	CASE("refresher: a called line configured uac leaves an open re-INVITE to the caller");
	fx_reset();
	cfg.channel[0].session_refresher = CFG_REFRESH_UAC;
	uas_up("call-sr5");
	CHECK(timer_armed(SIP_TIMER_CALL_WATCHDOG));
	rx(uas_in_dialog("INVITE", 2, "Supported: timer\r\nSession-Expires: 120\r\n",
			 sdp_body(T_PEER, 20000, "0 101", NULL)));
	CHECK(has(last_text(), "Session-Expires: 120;refresher=uac\r\n"));
	CHECK(timer_armed(SIP_TIMER_CALL_WATCHDOG));
	CHECK(!timer_armed(SIP_TIMER_CALL_SESSION));

	CASE("refresher: a calling line configured uac takes an open re-INVITE itself");
	fx_reset();
	uac_up("Session-Expires: 120;refresher=uac\r\n");
	rx(uac_in_dialog("INVITE", "peer-tag", 2, "Supported: timer\r\nSession-Expires: 120\r\n",
			 sdp_body(T_PEER, 20000, "0 101", NULL)));
	CHECK(has(last_text(), "Session-Expires: 120;refresher=uas\r\n"));
	CHECK_UINT(timer_delay(SIP_TIMER_CALL_SESSION), 60000);
	CHECK(!timer_armed(SIP_TIMER_CALL_WATCHDOG));

	/*
	 * With uac configured, the called line never refreshes. Its hold
	 * re-INVITE must still name the peer as the refresher, or each side
	 * waits for a refresh from the other.
	 */
	CASE("refresher: a called line's hold names the peer that refreshes uas");
	fx_reset();
	uas_up("call-sr6");
	CHECK(call_hold(0, 0));
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.method, "INVITE"));
	CHECK(has(last_text(), "Session-Expires: 120;refresher=uas\r\n"));
	reply(200, NULL, "Session-Expires: 120;refresher=uas\r\n",
	      sdp_body(T_PEER, 20000, "0 101", "a=recvonly\r\n"));
	CHECK_UINT(ev.reinvite_done, 1);
	CHECK(timer_armed(SIP_TIMER_CALL_WATCHDOG));
	CHECK(!timer_armed(SIP_TIMER_CALL_SESSION));

	CASE("refresher: a called line's refresh retried after a 422 still names itself uac");
	fx_reset();
	cfg.channel[0].session_refresher = CFG_REFRESH_UAS;
	uas_up("call-sr7");
	fake_advance(60000);
	reply(422, NULL, "Min-SE: 180\r\n", NULL);
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.method, "INVITE"));
	CHECK(has(last_text(), "Session-Expires: 180;refresher=uac\r\n"));
	CHECK(has(last_text(), "Min-SE: 180\r\n"));

	/*
	 * RFC 4028 9 Table 2 row 1: a UAC that does not list the timer option
	 * tag cannot refresh. Its request leaves one answer, uas, whatever the
	 * line prefers. A refresher the request names changes nothing: a uac
	 * answer would need a Require: timer this UAC cannot honor.
	 */
	CASE("refresher: a caller without the timer tag leaves the refresh with us");
	fx_reset();
	cfg.channel[0].session_refresher = CFG_REFRESH_UAC;
	rx(text_line_drop(invite_text("call-sa1", "Session-Expires: 1800\r\n",
				      sdp_body(T_PEER, 20000, "0", NULL)),
			  "Supported: timer\r\n"));
	CHECK(call_answer(0, 0));
	CHECK(has(last_text(), "Session-Expires: 120;refresher=uas\r\n"));
	CHECK(!has(last_text(), "Require: timer"));
	CHECK_UINT(timer_delay(SIP_TIMER_CALL_SESSION), 60000);

	CASE("refresher: and so does a re-INVITE without the tag");
	fx_reset();
	uas_up("call-sa2");
	CHECK(timer_armed(SIP_TIMER_CALL_WATCHDOG));
	rx(uas_in_dialog("INVITE", 2, "Session-Expires: 120\r\n",
			 sdp_body(T_PEER, 20000, "0 101", NULL)));
	CHECK(has(last_text(), "Session-Expires: 120;refresher=uas\r\n"));
	CHECK_UINT(timer_delay(SIP_TIMER_CALL_SESSION), 60000);
	CHECK(!timer_armed(SIP_TIMER_CALL_WATCHDOG));

	CASE("refresher: a caller without the tag that names itself uac still leaves it with us");
	fx_reset();
	rx(text_line_drop(invite_text("call-sa3", "Session-Expires: 1800;refresher=uac\r\n",
				      sdp_body(T_PEER, 20000, "0", NULL)),
			  "Supported: timer\r\n"));
	CHECK(call_answer(0, 0));
	CHECK(has(last_text(), "Session-Expires: 120;refresher=uas\r\n"));
	CHECK(!has(last_text(), "Require: timer"));
	CHECK_UINT(timer_delay(SIP_TIMER_CALL_SESSION), 60000);

	cfg.channel[0].session_refresher = CFG_REFRESH_UNSPEC;
	cfg.channel[0].session_max_expires = 0;
}

/*
 * RFC 4028 9: the interval in our 2xx can be lower than the one the request
 * names, but never lower than the Min-SE of the request, or 90 s without one.
 * It is never higher than the request value. A configured maximum below
 * either floor gives way to the floor. The one-day cap of the leg timers
 * still bounds the answer, even below a larger Min-SE.
 */
static void test_session_answer_floor(void)
{
	fx_reset();
	cfg.channel[0].session_max_expires = 120;

	CASE("answer floor: a maximum below the Min-SE of the request answers that Min-SE");
	rx(invite_text("call-af1", "Session-Expires: 1800\r\nMin-SE: 180\r\n",
		       sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(call_answer(0, 0));
	CHECK(has(last_text(), "Session-Expires: 180;"));

	CASE("answer floor: and so does an interval we ask for unasked");
	fx_reset();
	rx(invite_text("call-af2", "Min-SE: 180\r\n", sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(call_answer(0, 0));
	CHECK(has(last_text(), "Session-Expires: 180;"));

	CASE("answer floor: a re-INVITE gets the same floor");
	fx_reset();
	uas_up("call-af3");
	rx(uas_in_dialog("INVITE", 2, "Supported: timer\r\nSession-Expires: 1800\r\nMin-SE: 180\r\n",
			 sdp_body(T_PEER, 20000, "0 101", NULL)));
	CHECK(has(last_text(), "Session-Expires: 180;"));

	CASE("answer floor: an interval below the maximum is never raised");
	fx_reset();
	rx(invite_text("call-af4", "Session-Expires: 100\r\n", sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(call_answer(0, 0));
	CHECK(has(last_text(), "Session-Expires: 100;"));

	CASE("answer floor: a maximum below 90 s answers 90 s");
	fx_reset();
	cfg.channel[0].session_max_expires = 60;
	rx(invite_text("call-af5", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(call_answer(0, 0));
	CHECK(has(last_text(), "Session-Expires: 90;"));

	/* the two limits of the floor, one for a request RFC 4028 7.1 forbids
	 * and one for the one-day cap that every timer of the leg keeps */
	CASE("answer floor: a Min-SE above the interval of its own request gets that interval");
	fx_reset();
	cfg.channel[0].session_max_expires = 0;
	rx(invite_text("call-af6", "Session-Expires: 100\r\nMin-SE: 120\r\n",
		       sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(call_answer(0, 0));
	CHECK(has(last_text(), "Session-Expires: 100;"));

	CASE("answer floor: the one-day cap still bounds a Min-SE above it");
	fx_reset();
	rx(invite_text("call-af7", "Session-Expires: 100000\r\nMin-SE: 90000\r\n",
		       sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(call_answer(0, 0));
	CHECK(has(last_text(), "Session-Expires: 86400;"));

	cfg.channel[0].session_max_expires = 0;
}

/*
 * RFC 4028 7.4: a refresh of ours carries the largest Min-SE that a 422 or a
 * refresh request of the peer named on the dialog, and an interval no smaller
 * than that Min-SE. A later request or a 422 with a smaller Min-SE lowers
 * neither, and a request this side refuses still counts.
 */
static void test_session_min_se_keep(void)
{
	struct req_info ri;

	fx_reset();
	cfg.channel[0].session_max_expires = 120;
	cfg.channel[0].session_refresher = CFG_REFRESH_UAS;
	rx(invite_text("call-mk1", "Session-Expires: 1800\r\nMin-SE: 180\r\n",
		       sdp_body(T_PEER, 20000, "0 8 101", NULL)));
	CHECK(call_ring(0, 0));
	CHECK(call_answer(0, 0));
	rx(uas_in_dialog("ACK", 1, NULL, NULL));

	CASE("min-se keep: our refresh carries the Min-SE of the initial INVITE");
	fake_advance(90000);
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.method, "INVITE"));
	CHECK(has(last_text(), "Session-Expires: 180;refresher=uac\r\n"));
	CHECK(has(last_text(), "Min-SE: 180\r\n"));
	reply(200, NULL, "Require: timer\r\nSession-Expires: 180;refresher=uac\r\n",
	      sdp_body(T_PEER, 20000, "0 101", NULL));

	CASE("min-se keep: a smaller Min-SE in a later re-INVITE lowers neither value");
	rx(uas_in_dialog("INVITE", 2, "Supported: timer\r\nSession-Expires: 180\r\nMin-SE: 90\r\n",
			 sdp_body(T_PEER, 20000, "0 101", NULL)));
	CHECK(has(last_text(), "Session-Expires: 180;refresher=uas\r\n"));
	rx(uas_in_dialog("ACK", 2, NULL, NULL));
	fake_advance(90000);
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.method, "INVITE"));
	CHECK(has(last_text(), "Session-Expires: 180;refresher=uac\r\n"));
	CHECK(has(last_text(), "Min-SE: 180\r\n"));

	/*
	 * An answer can never exceed the interval of the request, so it can
	 * fall below the kept Min-SE. The refresh after it names the larger
	 * of the two (RFC 4028 7.4).
	 */
	CASE("min-se keep: after an answer below the kept Min-SE our refresh names that Min-SE");
	fx_reset();
	rx(invite_text("call-mk2", "Session-Expires: 1800\r\nMin-SE: 180\r\n",
		       sdp_body(T_PEER, 20000, "0 8 101", NULL)));
	CHECK(call_ring(0, 0));
	CHECK(call_answer(0, 0));
	rx(uas_in_dialog("ACK", 1, NULL, NULL));
	rx(uas_in_dialog("INVITE", 2, "Supported: timer\r\nSession-Expires: 120\r\nMin-SE: 90\r\n",
			 sdp_body(T_PEER, 20000, "0 101", NULL)));
	CHECK(has(last_text(), "Session-Expires: 120;refresher=uas\r\n"));
	rx(uas_in_dialog("ACK", 2, NULL, NULL));
	fake_advance(60000);
	CHECK(has(send_text((unsigned int)find_send("INVITE")),
		  "Session-Expires: 180;refresher=uac\r\n"));
	CHECK(has(send_text((unsigned int)find_send("INVITE")), "Min-SE: 180\r\n"));

	CASE("min-se keep: a 422 with a smaller Min-SE does not lower the kept one");
	reply(422, NULL, "Min-SE: 150\r\n", NULL);
	CHECK(req_of("INVITE", &ri));
	CHECK_UINT(ri.cseq, 2);
	CHECK(has(send_text((unsigned int)find_send("INVITE")), "Session-Expires: 180;"));
	CHECK(has(send_text((unsigned int)find_send("INVITE")), "Min-SE: 180\r\n"));

	CASE("min-se keep: the Min-SE of a refused re-INVITE still counts");
	fx_reset();
	uas_up("call-mk3");
	rx(uas_in_dialog("INVITE", 2, "Supported: timer\r\nSession-Expires: 600\r\nMin-SE: 300\r\n",
			 sdp_body(T_PEER, 20000, "99", "a=rtpmap:99 SPEEX/8000\r\n")));
	CHECK(has(last_text(), "SIP/2.0 488"));
	fake_advance(60000);
	CHECK(has(send_text((unsigned int)find_send("INVITE")),
		  "Session-Expires: 300;refresher=uac\r\n"));
	CHECK(has(send_text((unsigned int)find_send("INVITE")), "Min-SE: 300\r\n"));

	cfg.channel[0].session_refresher = CFG_REFRESH_UNSPEC;
	cfg.channel[0].session_max_expires = 0;
}

/*
 * RFC 4028 7.1: a UA that supports session timers lists the timer option tag
 * in each request. Each re-INVITE of ours lists timer and replaces, with or
 * without a session timer in force. It leaves out 100rel: a peer may answer
 * an INVITE that lists the tag with a reliable provisional (RFC 3262 3), and
 * this side sends no PRACK inside a dialog. The other in-dialog requests
 * carry no Supported header.
 */
static void test_reinvite_supported(void)
{
	fx_reset();
	cfg.channel[0].session_max_expires = 120;
	uac_up("Session-Expires: 120;refresher=uac\r\n");

	CASE("supported: a session refresh lists the timer option tag");
	fake_advance(60000);
	CHECK(has(last_text(), "Supported: " UA_SUPPORTED_REINVITE "\r\n"));
	cfg.channel[0].session_max_expires = 0;

	CASE("supported: a hold with no session timer lists it too");
	fx_reset();
	uac_up(NULL);
	CHECK(call_hold(0, 0));
	CHECK(has(last_text(), "Supported: " UA_SUPPORTED_REINVITE "\r\n"));

	CASE("supported: a re-INVITE offers no 100rel");
	CHECK(!has(last_text(), "100rel"));
	CHECK(has(send_text(0), "Supported: " UA_SUPPORTED_DEFAULT "\r\n"));

	CASE("supported: a BYE carries no Supported header");
	fx_reset();
	uac_up(NULL);
	CHECK(call_hangup(0, 0));
	CHECK(!has(send_text((unsigned int)find_send("BYE")), "Supported:"));
}

/*
 * A dialog takes one BYE. The line machine hangs a failed call up inside the
 * failure report (fsm.c connected_failed()), so the leg that reports can
 * already be clearing when the report returns. An ACK that never arrives can
 * also end a leg whose BYE is already out. Neither sends a second BYE.
 *
 * The first two cases follow a 120 s session whose
 * watchdog expired on a called line: the dialog carried BYEs with CSeq 2 and
 * 3. A retransmission keeps the CSeq and the branch, a second BYE does not.
 */
static void test_abandon_one_bye(void)
{
	struct req_info first, last;

	fx_reset();
	cfg.channel[0].session_max_expires = 120;
	uac_up("Session-Expires: 120;refresher=uas\r\n");
	ev.failed_hangs_up = true;

	CASE("one bye: a calling line's watchdog hung up inside the report sends one BYE");
	fake_advance(88000);
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(count_send("BYE"), 1);

	CASE("one bye: and so does a called line's");
	fx_reset();
	cfg.channel[0].session_refresher = CFG_REFRESH_UAC;
	uas_up("call-ob2");
	ev.failed_hangs_up = true;
	fake_advance(88000);
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(count_send("BYE"), 1);
	cfg.channel[0].session_refresher = CFG_REFRESH_UNSPEC;
	cfg.channel[0].session_max_expires = 0;

	CASE("one bye: a missing ACK behind our own BYE sends no second one");
	fx_reset();
	rx(invite_text("call-ob3", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(call_answer(0, 0));
	fake_advance(1000);
	CHECK(call_hangup(0, 0));
	CHECK(req_of("BYE", &first));
	/* past the 64*T1 of the 2xx, short of timer F of the BYE */
	fake_advance(31500);
	CHECK_UINT(ua.stat.ack_missing, 1);
	CHECK(req_of("BYE", &last));
	CHECK_UINT(last.cseq, first.cseq);
	CHECK(!strcmp(last.branch, first.branch));
}

static void test_timer_c(void)
{
	fx_reset();
	cfg.channel[0].timer_c_ms = 5000;

	CASE("timer C: the configured bound is armed");
	CHECK(call_dial(0, 0, "1002", true));
	CHECK_UINT(timer_delay(SIP_TIMER_CALL_C), 5000);

	CASE("timer C: with no provisional the INVITE is abandoned without a CANCEL");
	fake_advance(5000);
	CHECK(find_send("CANCEL") < 0);
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.cls, CALL_FAIL_CONGESTION);
	CHECK(slot_is(0));

	CASE("timer C: after a provisional the setup is cancelled");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	reply(100, NULL, NULL, NULL);
	fake_advance(5000);
	CHECK(find_send("CANCEL") > 0);
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.cls, CALL_FAIL_CONGESTION);

	CASE("timer C: the default bound applies when the line configures none");
	fx_reset();
	cfg.channel[0].timer_c_ms = 0;
	CHECK(call_dial(0, 0, "1002", true));
	CHECK_UINT(timer_delay(SIP_TIMER_CALL_C), 180000);
}

static void test_uas(void)
{
	char tag180[64], tag487[64];

	fx_reset();

	CASE("uas: an INVITE reports an incoming call with its caller id");
	rx(invite_text("call-1", NULL, sdp_body(T_PEER, 20000, "0 8 101", NULL)));
	CHECK_UINT(ev.incoming, 1);
	CHECK(!strcmp(ev.cid_number, "1002"));
	CHECK(!strcmp(ev.cid_name, "Alice"));
	CHECK_UINT(ev.slot, 0);

	CASE("uas: no distinctive ring without an Alert-Info");
	CHECK_UINT(ev.ring_id, 0);

	CASE("uas: nothing is answered before the FSM says so");
	CHECK_UINT(fk.sends, 0);

	CASE("uas: ringing sends a 180 with our tag");
	CHECK(call_ring(0, 0));
	CHECK_UINT(fk.sends, 1);
	CHECK(has(send_text(0), "SIP/2.0 180"));
	CHECK(resp_to_tag(0, tag180, sizeof(tag180)));
	CHECK(tag180[0] != 0);

	CASE("uas: the 180 echoes the Record-Route and carries our Contact");
	CHECK(has(send_text(0), "Record-Route: <sip:proxy.example.net;lr>"));
	CHECK(has(send_text(0), "Contact: <sip:1001@" T_HOST ":5060>"));

	CASE("uas: a second ring changes nothing");
	CHECK(call_ring(0, 0));
	CHECK_UINT(fk.sends, 1);

	CASE("uas: the automatic 100 stays off the wire after our own 180");
	fake_advance(300);
	CHECK_UINT(fk.sends, 1);

	CASE("uas: answering sends a 2xx with the answer");
	CHECK(call_answer(0, 0));
	CHECK_UINT(fk.sends, 2);
	CHECK(has(send_text(1), "SIP/2.0 200"));
	CHECK(has(send_text(1), "m=audio 10000 RTP/AVP 0 101"));
	CHECK(has(send_text(1), "Record-Route: <sip:proxy.example.net;lr>"));
	CHECK(has(send_text(1), "Contact: <sip:1001@" T_HOST ":5060>"));

	CASE("uas: the 2xx keeps the tag the 180 used");
	CHECK(resp_to_tag(1, tag487, sizeof(tag487)));
	CHECK(!strcmp(tag180, tag487));

	CASE("uas: the 2xx is handed to its retransmission owner");
	CHECK_UINT(ua_accept_count(&ua), 1);

	CASE("uas: media starts with the 2xx, not with the ACK");
	CHECK_UINT(med.config_calls, 1);
	CHECK_UINT(med.start_calls, 1);
	CHECK_UINT(med.last.remote.port, 20000);
	CHECK_UINT(med.last.codec, HAL_CODEC_PCMU);

	CASE("uas: no answered event is owed on the answering side");
	CHECK_UINT(ev.answered, 0);

	CASE("uas: the ACK completes the exchange without reprogramming");
	rx(uas_in_dialog("ACK", 1, NULL, NULL));
	CHECK_UINT(med.config_calls, 1);
	CHECK_UINT(med.start_calls, 1);

	CASE("uas: a retransmitted ACK never reaches the call layer");
	rx(uas_in_dialog("ACK", 1, NULL, NULL));
	CHECK_UINT(med.config_calls, 1);
	CHECK_UINT(ev.failed, 0);

	CASE("uas: the slot is occupied while the call runs");
	CHECK(slot_is(1));

	CASE("uas: an Alert-Info names the distinctive ring");
	fx_reset();
	rx(invite_text("call-2", "Alert-Info: <http://127.0.0.1/Bellcore-dr3>\r\n",
		       sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK_UINT(ev.incoming, 1);
	CHECK_UINT(ev.ring_id, 3);

	CASE("uas: a reject answers the code the FSM chose and frees the slot");
	CHECK(call_reject(0, 0, 486));
	CHECK(has(last_text(), "SIP/2.0 486"));
	CHECK_UINT(ev.failed, 0);
	CHECK(slot_is(0));

	CASE("uas: a rejected call closes a record that names the cause");
	CHECK_UINT(ev.ended, 1);
	CHECK(ev.end.inbound);
	CHECK(ev.end.failed);
	CHECK_UINT(ev.end.cls, CALL_FAIL_BUSY);

	CASE("uas: an INVITE with no common codec is answered 488");
	fx_reset();
	rx(invite_text("call-3", NULL,
		       sdp_body(T_PEER, 20000, "99", "a=rtpmap:99 SPEEX/8000\r\n")));
	CHECK(call_ring(0, 0));
	CHECK(!call_answer(0, 0));
	CHECK(has(last_text(), "SIP/2.0 488"));
	CHECK_UINT(ev.failed, 1);
	CHECK(slot_is(0));

	CASE("uas: an unparseable body is refused before the phone rings");
	fx_reset();
	rx(invite_text("call-4", NULL, "this is not sdp\r\n"));
	CHECK_UINT(ev.incoming, 0);
	CHECK(has(last_text(), "SIP/2.0 488"));
	CHECK(slot_is(0));

	CASE("uas: answering without ringing first still works");
	fx_reset();
	rx(invite_text("call-5", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(call_answer(0, 0));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(med.start_calls, 1);

	CASE("uas: a session interval below our floor is refused with 422");
	fx_reset();
	cfg.channel[0].session_min_interval = 900;
	rx(invite_text("call-6", "Session-Expires: 120;refresher=uac\r\n",
		       sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK_UINT(ev.incoming, 0);
	CHECK(has(last_text(), "SIP/2.0 422"));
	CHECK(has(last_text(), "Min-SE: 900"));
	CHECK(slot_is(0));
	cfg.channel[0].session_min_interval = 0;

	CASE("uas: an accepted session interval is echoed in the 2xx");
	fx_reset();
	rx(invite_text("call-7", "Session-Expires: 1800;refresher=uac\r\n",
		       sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(call_answer(0, 0));
	CHECK(has(last_text(), "Session-Expires: 1800;refresher=uac"));
	CHECK(has(last_text(), "Require: timer"));

	CASE("uas: the peer refresher arms our watchdog");
	CHECK(timer_armed(SIP_TIMER_CALL_WATCHDOG));
	CHECK(!timer_armed(SIP_TIMER_CALL_SESSION));

	CASE("uas: we refresh when the peer named us");
	fx_reset();
	rx(invite_text("call-8", "Session-Expires: 1800;refresher=uas\r\n",
		       sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(call_answer(0, 0));
	CHECK(has(last_text(), "Session-Expires: 1800;refresher=uas"));
	CHECK_UINT(timer_delay(SIP_TIMER_CALL_SESSION), 900000);

	CASE("uas: no ACK for the 2xx ends the session (RFC 3261 13.3.1.4)");
	fx_reset();
	rx(invite_text("call-9", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(call_ring(0, 0));
	CHECK(call_answer(0, 0));
	fake_advance(32000);
	CHECK(find_send("BYE") > 0);
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.cls, CALL_FAIL_QUIET);

	/* the registration gates new calls only; a call that rang while the
	 * binding was live is the subscriber's to take */
	CASE("uas: a call that rang while registered can still be answered with no live registration");
	fx_reset();
	rx(invite_text("call-10", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(call_ring(0, 0));
	fk.registered = false;
	CHECK(call_answer(0, 0));
	CHECK(has(last_text(), "SIP/2.0 200"));
}

/*
 * The dialog pool of the account, filled to its bound. No other case of this
 * suite fills it, and a leg cannot fill it by itself: a line carries two legs
 * and each releases its dialog when it ends.
 */
static void dialog_pool_fill(void)
{
	unsigned int i;

	for (i = 0; i < SIP_DIALOG_MAX; i++) {
		char id[32];
		struct sip_msg *m;

		snprintf(id, sizeof(id), "call-fill-%u", i);
		m = msg_take(invite_text(id, NULL, NULL));
		CHECK(m != NULL);
		if (m)
			CHECK(sip_dialog_uas_create(&dlgs, m, id, &fk.peer, NULL) != NULL);
	}

	CHECK_UINT(sip_dialog_count(&dlgs), SIP_DIALOG_MAX);
}

/*
 * A 180 that this side cannot build or send. Each of the four refusals of
 * call_ring() answers the caller 500 and fails the leg. The bell therefore
 * never rings for a call that the caller already left. A ring with no 180
 * leaves the caller with the automatic 100 alone while the phone rings.
 */
static void test_ring_refuse(void)
{
	uint32_t txn;

	CASE("ring refuse: a full dialog pool answers the INVITE 500 and rings "
	     "nothing");
	fx_reset();
	dialog_pool_fill();
	rx(invite_text("call-rr1", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK_UINT(ev.incoming, 1);
	CHECK(!call_ring(0, 0));
	CHECK(has(last_text(), "SIP/2.0 500"));

	CASE("ring refuse: the refused leg reports congestion exactly once");
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.cls, CALL_FAIL_CONGESTION);

	CASE("ring refuse: the refused leg frees its slot");
	CHECK(slot_is(0));
	CHECK_UINT(ev.ended, 1);

	CASE("ring refuse: no 180 ever reached the wire");
	CHECK(find_send("SIP/2.0 180") < 0);

	CASE("ring refuse: the 500 is the only response, the automatic 100 "
	     "never leaves");
	CHECK_UINT(fk.sends, 1);
	fake_advance(300);
	CHECK_UINT(fk.sends, 1);
	CHECK_UINT(ua.stat.trying_sent, 0);
	/* the leg took its pending 100 with it (ua_owner_release), so the
	 * timer of that 100 never runs and counts no skip either */
	CHECK_UINT(ua.stat.trying_skipped, 0);

	CASE("ring refuse: a leg whose server transaction died refuses without "
	     "a response");
	fx_reset();
	txn = rx_direct(0, invite_text("call-rr2", NULL,
				       sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK_UINT(ev.incoming, 1);
	sip_txn_abort(sip_txn_get(&mgr, txn));
	CHECK(!call_ring(0, 0));
	CHECK_UINT(fk.sends, 0);
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.cls, CALL_FAIL_CONGESTION);
	CHECK(slot_is(0));
}

static void test_uas_offerless(void)
{
	fx_reset();

	CASE("offerless: the 2xx carries our offer");
	rx(invite_text("call-o1", NULL, NULL));
	CHECK_UINT(ev.incoming, 1);
	CHECK(call_ring(0, 0));
	CHECK(call_answer(0, 0));
	CHECK(has(last_text(), "m=audio 10000 RTP/AVP 0 8 18 101"));

	CASE("offerless: media waits for the answer in the ACK");
	CHECK_UINT(med.config_calls, 0);
	CHECK_UINT(med.start_calls, 0);

	CASE("offerless: the answer in the ACK programs the stream");
	rx(uas_in_dialog("ACK", 1, NULL, sdp_body(T_PEER, 20000, "8", NULL)));
	CHECK_UINT(med.config_calls, 1);
	CHECK_UINT(med.start_calls, 1);
	CHECK_UINT(med.last.codec, HAL_CODEC_PCMA);
	CHECK_UINT(med.last.codec_pt, 8);
	CHECK_UINT(med.last.remote.port, 20000);

	CASE("offerless: an ACK without the answer clears the call quietly");
	fx_reset();
	rx(invite_text("call-o2", NULL, NULL));
	CHECK(call_ring(0, 0));
	CHECK(call_answer(0, 0));
	rx(uas_in_dialog("ACK", 1, NULL, NULL));
	CHECK(find_send("BYE") > 0);
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.cls, CALL_FAIL_QUIET);

	CASE("offerless: an ACK for another exchange commits nothing");
	fx_reset();
	rx(invite_text("call-o4", NULL, NULL));
	CHECK(call_answer(0, 0));
	{
		struct sip_msg *m = msg_take(uas_in_dialog("ACK", 9, NULL,
							   sdp_body(T_PEER, 20000, "8", NULL)));

		CHECK(m != NULL);
		if (m)
			call_rx_ack(0, m);
	}
	CHECK_UINT(med.config_calls, 0);
	rx(uas_in_dialog("ACK", 1, NULL, sdp_body(T_PEER, 20000, "8", NULL)));
	CHECK_UINT(med.config_calls, 1);

	CASE("offerless: an unusable answer in the ACK clears the call too");
	fx_reset();
	rx(invite_text("call-o3", NULL, NULL));
	CHECK(call_answer(0, 0));
	rx(uas_in_dialog("ACK", 1, NULL,
			 sdp_body(T_PEER, 20000, "99", "a=rtpmap:99 SPEEX/8000\r\n")));
	CHECK(find_send("BYE") > 0);
	CHECK_UINT(ev.cls, CALL_FAIL_QUIET);
}

static void test_inbound_cancel(void)
{
	char tag180[64], tag180b[64], tag487[64];
	unsigned int i;

	fx_reset();
	rx(invite_text("call-c1", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(call_ring(0, 0));

	CASE("cancel in: the CANCEL is answered 200 and the INVITE 487");
	rx(cancel_text("call-c1"));
	CHECK(has(send_text(1), "SIP/2.0 200"));
	CHECK(has(send_text(2), "SIP/2.0 487"));

	CASE("cancel in: the 487 carries the tag the 180 used");
	CHECK(resp_to_tag(0, tag180, sizeof(tag180)));
	CHECK(resp_to_tag(2, tag487, sizeof(tag487)));
	CHECK(!strcmp(tag180, tag487));

	CASE("cancel in: the leg reports the cancel and frees its slot");
	CHECK_UINT(ev.remote_cancel, 1);
	CHECK_UINT(ev.failed, 0);
	CHECK(slot_is(0));

	CASE("cancel in: a CANCEL for nothing is answered 481");
	fx_reset();
	rx(cancel_text("no-such-call"));
	CHECK(has(last_text(), "SIP/2.0 481"));
	CHECK_UINT(ev.remote_cancel, 0);

	CASE("cancel in: a CANCEL before any provisional still cancels");
	fx_reset();
	rx(invite_text("call-c2", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	rx(cancel_text("call-c2"));
	CHECK_UINT(ev.remote_cancel, 1);
	CHECK(slot_is(0));

	/*
	 * A field sequence: the caller gives up and calls
	 * again at once. Nothing guards the new call, so the second INVITE
	 * takes a leg, a dialog and a 180 of its own.
	 */
	CASE("cancel in: a second INVITE right behind the 487 rings and gets "
	     "its own 180");
	fx_reset();
	rx(invite_text("call-c3", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(call_ring(0, 0));
	rx(cancel_text("call-c3"));
	CHECK(has(send_text(2), "SIP/2.0 487"));
	rx(invite_text("call-c4", NULL, sdp_body(T_PEER, 20002, "0", NULL)));
	CHECK_UINT(ev.incoming, 2);
	CHECK(call_ring(0, 0));
	CHECK_UINT(count_send("SIP/2.0 180"), 2);

	CASE("cancel in: the second 180 carries a tag of its own");
	CHECK(resp_to_tag(0, tag180, sizeof(tag180)));
	CHECK(resp_to_tag((unsigned int)find_send("SIP/2.0 180"), tag180b,
			  sizeof(tag180b)));
	CHECK(tag180b[0] != 0);
	CHECK(strcmp(tag180, tag180b) != 0);

	/* nine is one more than the dialog pool holds, so a dialog the cancel
	 * failed to release would refuse the ninth ring */
	CASE("cancel in: nine cancelled calls in a row each get a 180");
	fx_reset();
	for (i = 0; i < 9; i++) {
		char id[32];

		snprintf(id, sizeof(id), "call-c9-%u", i);
		rx(invite_text(id, NULL, sdp_body(T_PEER, 20000, "0", NULL)));
		CHECK(call_ring(0, 0));
		rx(cancel_text(id));
	}
	CHECK_UINT(count_send("SIP/2.0 180"), 9);
	CHECK_UINT(ev.remote_cancel, 9);

	CASE("cancel in: the dialog pool is empty again after the ninth");
	CHECK_UINT(sip_dialog_count(&dlgs), 0);

	CASE("cancel in: and the pool denied nothing along the way");
	CHECK_UINT(dlgs.stat.denied, 0);

	CASE("cancel in: an INVITE retransmitted while the leg rings repeats the 180");
	fx_reset();
	rx(invite_text("call-c10", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(call_ring(0, 0));
	rx(invite_text("call-c10", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK_UINT(count_send("SIP/2.0 180"), 2);

	CASE("cancel in: and it opens no second leg");
	CHECK_UINT(ev.incoming, 1);
	CHECK(slot_is(1));
	CHECK_UINT(sip_dialog_count(&dlgs), 1);
}

static void test_inbound_bye(void)
{
	fx_reset();
	uas_up("call-b1");

	CASE("bye in: the peer BYE is answered 200");
	rx(uas_in_dialog("BYE", 2, NULL, NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));

	CASE("bye in: the stream stops and the FSM hears the clear");
	CHECK_UINT(med.stop_calls, 1);
	CHECK_UINT(ev.remote_bye, 1);
	CHECK_UINT(ev.failed, 0);

	CASE("bye in: the slot is free again");
	CHECK(slot_is(0));

	CASE("bye in: an old CSeq is refused with 500 (RFC 3261 12.2.2)");
	fx_reset();
	uas_up("call-b2");
	rx(uas_in_dialog("BYE", 1, NULL, NULL));
	CHECK(has(last_text(), "SIP/2.0 500"));
	CHECK_UINT(ev.remote_bye, 0);
	CHECK(slot_is(1));

	CASE("bye in: an in-dialog request with no dialog is refused 481");
	fx_reset();
	rx(dialog_req_text("BYE", 9, "no-such-dialog", "peer", "us", NULL, NULL));
	CHECK(has(last_text(), "SIP/2.0 481"));

	/*
	 * RFC 3261 15.1.2: a BYE on the early dialog ends it, and the INVITE
	 * behind the 180 still needs its final response.
	 */
	CASE("bye in: a BYE on the early dialog is answered 200 and the INVITE 487");
	fx_reset();
	rx(invite_text("call-b5", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(call_ring(0, 0));
	{
		char call_id[160], peer_tag[64], our_tag[64];

		CHECK(resp_tags_at(find_send("SIP/2.0 180"), call_id, sizeof(call_id),
				   peer_tag, sizeof(peer_tag), our_tag, sizeof(our_tag)));
		rx(dialog_req_text("BYE", 2, call_id, peer_tag, our_tag, NULL, NULL));
	}
	CHECK(has(send_text(1), "SIP/2.0 200"));
	CHECK(has(send_text(1), "CSeq: 2 BYE"));
	CHECK(has(send_text(2), "SIP/2.0 487"));
	CHECK(has(send_text(2), "CSeq: 1 INVITE"));

	CASE("bye in: the 487 of the early BYE carries the tag the 180 used");
	{
		char tag180[64], tag487[64];

		CHECK(resp_to_tag(0, tag180, sizeof(tag180)));
		CHECK(resp_to_tag(2, tag487, sizeof(tag487)));
		CHECK(!strcmp(tag180, tag487));
	}

	CASE("bye in: the early BYE reaches the FSM as a clear and frees the slot");
	CHECK_UINT(ev.remote_bye, 1);
	CHECK_UINT(ev.failed, 0);
	CHECK(slot_is(0));

	CASE("bye out: our BYE frees the slot when its transaction ends");
	fx_reset();
	uas_up("call-b3");
	CHECK(call_hangup(0, 0));
	CHECK(find_send("BYE") > 0);
	reply(200, NULL, NULL, NULL);
	fake_advance(6000);
	CHECK(slot_is(0));

	/*
	 * The crossing BYE (call.h states the transfer instance of the same
	 * rule in the last-word clause of cb.xfer_done). The subscriber hung
	 * up, so this leg reported its own end and the FSM may have given the
	 * slot another role already: the peer's own BYE, which crossed ours on
	 * the wire, is answered and ends the leg, and it reaches the machine
	 * nowhere.
	 */
	CASE("crossing bye: a peer BYE behind a local hangup is answered and reported nowhere");
	fx_reset();
	uac_up(NULL);
	CHECK(call_hangup(0, 0));
	CHECK(find_send("BYE") > 0);
	rx(uac_in_dialog("BYE", "peer-tag", 2, NULL, NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(ev.remote_bye, 0);
	CHECK_UINT(ev.failed, 0);
	CHECK_UINT(med.stop_calls, 1);

	CASE("crossing bye: and the record of that leg is one and intact");
	CHECK_UINT(ev.ended, 1);
	CHECK_UINT(ev.ends[0][0].count, 1);
	CHECK(ev.end.answered);
	CHECK(!ev.end.failed);
	CHECK(ev.end.cleared);
	CHECK(!ev.end.transferred);
	CHECK(slot_is(0));

	CASE("crossing bye: an inbound leg the subscriber hung up takes it the same way");
	fx_reset();
	uas_up("call-b4");
	CHECK(call_hangup(0, 0));
	rx(uas_in_dialog("BYE", 2, NULL, NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(ev.remote_bye, 0);
	CHECK_UINT(ev.failed, 0);
	CHECK_UINT(ev.ended, 1);
	CHECK(slot_is(0));

	/* the teardown reports the quiet class itself, so the leg it BYEd owes
	 * the machine nothing more either */
	CASE("crossing bye: a peer BYE behind a line teardown reports no second clear");
	fx_reset();
	uac_up(NULL);
	calls_line_teardown(0);
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.cls, CALL_FAIL_QUIET);
	rx(uac_in_dialog("BYE", "peer-tag", 2, NULL, NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(ev.remote_bye, 0);
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.ended, 1);
	CHECK(slot_is(0));
}

/*
 * The INVITE server transaction of an inbound leg, seen through the path
 * that call_txn_done() takes for it. A transport error under a leg that never
 * answered fails that leg. The two normal ends of the transaction leave the
 * leg as they found it. They are the 2xx that hands the retransmission to the
 * UA core, and the timer H of a rejection that the peer never acknowledged.
 */
static void test_server_txn(void)
{
	CASE("server txn: a plain 180 the peer asks for again goes out twice");
	fx_reset();
	rx(invite_text("call-s1", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(call_ring(0, 0));
	CHECK(has(send_text(0), "SIP/2.0 180"));
	CHECK(!has(send_text(0), "Require: 100rel"));
	rx(invite_text("call-s1", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK_UINT(count_send("SIP/2.0 180"), 2);
	CHECK_UINT(ev.failed, 0);

	CASE("server txn: a copy the transport refuses fails the ringing leg");
	fk.send_fail = true;
	rx(invite_text("call-s1", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	fk.send_fail = false;
	CHECK_UINT(tr.ist_fail_lines, 1);
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.cls, CALL_FAIL_CONGESTION);

	CASE("server txn: that leg frees its slot and leaves no timer armed");
	CHECK_UINT(ev.ended, 1);
	CHECK(slot_is(0));
	CHECK(!timer_armed(SIP_TIMER_CALL_REL));
	CHECK_UINT(fk.timers, 0);

	CASE("server txn: an answered call outlives that transaction");
	fx_reset();
	uas_up("call-s2");
	fake_advance(40000);
	CHECK_UINT(ev.failed, 0);
	CHECK_UINT(ev.ended, 0);
	CHECK(slot_is(1));

	CASE("server txn: and a peer BYE behind it still finds the leg");
	rx(uas_in_dialog("BYE", 2, NULL, NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(ev.remote_bye, 1);
	CHECK_UINT(ev.failed, 0);
	CHECK(slot_is(0));

	/* timer H ends the transaction of a rejection the peer never ACKed
	 * (RFC 3261 17.2.1), and the leg went with the 486 */
	CASE("server txn: a 486 the peer never ACKs reports one end alone");
	fx_reset();
	rx(invite_text("call-s3", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(call_ring(0, 0));
	CHECK(call_reject(0, 0, 486));
	CHECK(has(last_text(), "SIP/2.0 486"));
	CHECK_UINT(ev.ended, 1);
	CHECK_UINT(ev.failed, 0);
	fake_advance(40000);
	CHECK_UINT(ev.ended, 1);
	CHECK_UINT(ev.failed, 0);
	CHECK(slot_is(0));
	CHECK_UINT(fk.timers, 0);
}

static void test_reinvite_in(void)
{
	fx_reset();
	uas_up("call-r1");

	CASE("re-INVITE in: a hold offer is answered recvonly");
	rx(uas_in_dialog("INVITE", 2, NULL, sdp_body(T_PEER, 20000, "0", "a=sendonly\r\n")));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(has(last_text(), "a=recvonly"));

	CASE("re-INVITE in: hold is reported only once the ACK commits it");
	CHECK_UINT(ev.hold_changed, 0);
	rx(uas_in_dialog("ACK", 2, NULL, NULL));
	CHECK_UINT(ev.hold_changed, 1);
	CHECK(ev.held);

	CASE("re-INVITE in: the stream direction follows the negotiation");
	CHECK_UINT(med.config_calls, 2);
	CHECK_UINT(med.last.direction, HAL_MEDIA_RECVONLY);

	CASE("re-INVITE in: a resume is reported the same way");
	rx(uas_in_dialog("INVITE", 3, NULL, sdp_body(T_PEER, 20000, "0", "a=sendrecv\r\n")));
	rx(uas_in_dialog("ACK", 3, NULL, NULL));
	CHECK_UINT(ev.hold_changed, 2);
	CHECK(!ev.held);
	CHECK_UINT(med.last.direction, HAL_MEDIA_SENDRECV);

	CASE("re-INVITE in: a moved remote address is re-applied to the stream");
	rx(uas_in_dialog("INVITE", 4, NULL, sdp_body("203.0.113.9", 30000, "0", NULL)));
	rx(uas_in_dialog("ACK", 4, NULL, NULL));
	CHECK_UINT(med.config_calls, 4);
	CHECK_UINT(med.last.remote.port, 30000);
	CHECK_UINT(med.last.remote.addr[0], 203);

	CASE("re-INVITE in: an unchanged description is not re-applied");
	rx(uas_in_dialog("INVITE", 5, NULL, sdp_body("203.0.113.9", 30000, "0", NULL)));
	rx(uas_in_dialog("ACK", 5, NULL, NULL));
	CHECK_UINT(med.config_calls, 4);

	CASE("re-INVITE in: T.38 is refused at port 0 and the audio survives");
	rx(uas_in_dialog("INVITE", 6, NULL,
			 sdp_body("203.0.113.9", 30000, "0", "m=image 30002 udptl t38\r\n")));
	CHECK(has(last_text(), "m=image 0 udptl t38"));
	CHECK(has(last_text(), "m=audio 10000"));
	rx(uas_in_dialog("ACK", 6, NULL, NULL));

	CASE("re-INVITE in: an offer with no common codec is refused 488");
	rx(uas_in_dialog("INVITE", 7, NULL,
			 sdp_body(T_PEER, 20000, "99", "a=rtpmap:99 SPEEX/8000\r\n")));
	CHECK(has(last_text(), "SIP/2.0 488"));

	CASE("re-INVITE in: the refused offer left the media alone");
	CHECK_UINT(med.last.remote.port, 30000);
	CHECK_UINT(med.config_calls, 4);

	CASE("re-INVITE in: the call is still up after the refusal");
	rx(uas_in_dialog("INVITE", 8, NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 200"));
	rx(uas_in_dialog("ACK", 8, NULL, NULL));
	CHECK_UINT(med.last.remote.port, 20000);

	CASE("re-INVITE in: an offerless re-INVITE takes our offer and its ACK answer");
	rx(uas_in_dialog("INVITE", 9, NULL, NULL));
	CHECK(has(last_text(), "m=audio 10000 RTP/AVP 0 8 18 101"));
	rx(uas_in_dialog("ACK", 9, NULL, sdp_body(T_PEER, 22000, "8", NULL)));
	CHECK_UINT(med.last.remote.port, 22000);
	CHECK_UINT(med.last.codec, HAL_CODEC_PCMA);

	CASE("re-INVITE in: a session interval on the re-INVITE re-arms the timer");
	fx_reset();
	uas_up("call-r2");
	rx(uas_in_dialog("INVITE", 2, "Supported: timer\r\nSession-Expires: 1800;refresher=uac\r\n",
			 sdp_body(T_PEER, 20000, "0", NULL)));
	rx(uas_in_dialog("ACK", 2, NULL, NULL));
	CHECK(timer_armed(SIP_TIMER_CALL_WATCHDOG));

	/*
	 * The pre-ring predicate runs on the initial INVITE alone. A re-offer
	 * that names a host is still the answer layer's refusal, and the 488
	 * plus the rollback leave the running session exactly as it was
	 * (RFC 3261 14.2): a call in progress is not dropped for a description
	 * we did not accept.
	 */
	CASE("re-INVITE in: a re-offer whose c= names a host is refused 488");
	fx_reset();
	uas_up("call-r3");
	CHECK_UINT(med.config_calls, 1);
	rx(uas_in_dialog("INVITE", 2, NULL, sdp_body("sip.example.com", 30000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 488"));

	CASE("re-INVITE in: the refused re-offer left the running call alone");
	CHECK_UINT(med.config_calls, 1);
	CHECK_UINT(med.last.remote.port, 20000);
	CHECK_UINT(med.stop_calls, 0);
	CHECK_UINT(ev.failed, 0);
	CHECK(slot_is(1));
}

static void test_reinvite_out(void)
{
	struct slot_view sv;
	struct req_info ri;
	int ridx;

	fx_reset();
	uac_up(NULL);

	/*
	 * The hold silence (hal.h, hal_media.tx_silence). a=sendonly says who
	 * may send and not what: the flag beside it is what stops the stream
	 * encoding the room, and the dial tone the FXS hybrid leaks into it,
	 * for as long as this side holds the call.
	 */
	CASE("hold: an answered call sends its own uplink before any hold");
	CHECK(!med.last.tx_silence);

	CASE("hold: our re-INVITE offers sendonly on the next CSeq");
	CHECK(call_hold(0, 0));
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.method, "INVITE"));
	CHECK(has(ri.body, "a=sendonly"));
	CHECK_UINT(ri.cseq, 2);
	ridx = find_send("INVITE");

	CASE("hold: a peer re-INVITE while ours is in flight gets 491");
	rx(uac_in_dialog("INVITE", "peer-tag", 1, NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 491"));

	CASE("hold: the 2xx completes the operation");
	reply_at((unsigned int)ridx, 200, NULL, NULL,
		 sdp_body(T_PEER, 20000, "0", "a=recvonly\r\n"));
	CHECK_UINT(ev.reinvite_done, 1);
	CHECK(ev.reinvite_ok);

	CASE("hold: the stream turns send only and the exchange is acknowledged");
	CHECK_UINT(med.last.direction, HAL_MEDIA_SENDONLY);
	CHECK_UINT(count_send("ACK"), 2);

	CASE("hold: the held stream stops sending the local uplink");
	CHECK(med.last.tx_silence);

	CASE("resume: the offer goes back to sendrecv");
	CHECK(call_resume(0, 0));
	CHECK(last_req(&ri));
	CHECK(!has(ri.body, "a=sendonly"));
	CHECK_UINT(ri.cseq, 3);

	CASE("resume: a 488 rolls back and reports the failure");
	reply(488, NULL, NULL, NULL);
	CHECK_UINT(ev.reinvite_done, 2);
	CHECK(!ev.reinvite_ok);
	CHECK_UINT(ev.failed, 0);

	CASE("resume: the committed media survived the rejected offer");
	CHECK_UINT(med.last.direction, HAL_MEDIA_SENDONLY);
	/* the rollback put the hold state back, so the leg is still held and
	 * still silent: a refused resume must not open the uplink */
	CHECK(med.last.tx_silence);

	CASE("resume: the leg still takes a new operation after the failure");
	CHECK(call_resume(0, 0));
	CHECK(last_req(&ri));
	CHECK_UINT(ri.cseq, 4);
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "0", NULL));
	CHECK_UINT(ev.reinvite_done, 3);
	CHECK(ev.reinvite_ok);
	CHECK_UINT(med.last.direction, HAL_MEDIA_SENDRECV);

	CASE("resume: the resumed stream sends the local uplink again");
	CHECK(!med.last.tx_silence);

	CASE("hold: a 491 to our re-INVITE arms the glare timer and retries");
	fx_reset();
	uac_up(NULL);
	CHECK(call_hold(0, 0));
	reply(491, NULL, NULL, NULL);
	CHECK(timer_armed(SIP_TIMER_GLARE));
	CHECK_UINT(ev.reinvite_done, 0);
	fake_advance(4000);
	CHECK(req_of("INVITE", &ri));
	CHECK_UINT(ri.cseq, 3);
	CHECK(has(ri.body, "a=sendonly"));

	CASE("hold: the retried hold completes normally");
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "0", "a=recvonly\r\n"));
	CHECK_UINT(ev.reinvite_done, 1);
	CHECK(ev.reinvite_ok);

	/* the word an operator and the arc.voip mapper read: the leg carries
	 * media throughout the wait, so the dump says so */
	CASE("hold: the leg reports active while its 491 retry waits");
	fx_reset();
	uac_up(NULL);
	CHECK(call_hold(0, 0));
	reply(491, NULL, NULL, NULL);
	CHECK(timer_armed(SIP_TIMER_GLARE));
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.state, "active"));

	CASE("hold: a glare timer that outlives its exchange sends nothing");
	fx_reset();
	uac_up(NULL);
	CHECK(call_hold(0, 0));
	reply(491, NULL, NULL, NULL);
	CHECK(timer_armed(SIP_TIMER_GLARE));
	CHECK(call_hangup(0, 0));
	{
		unsigned int before = count_send("INVITE");

		fake_advance(4000);
		CHECK_UINT(count_send("INVITE"), before);
	}

	CASE("hold: a 481 to our re-INVITE clears the call");
	fx_reset();
	uac_up(NULL);
	CHECK(call_hold(0, 0));
	reply(481, NULL, NULL, NULL);
	CHECK(find_send("BYE") > 0);
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.cls, CALL_FAIL_QUIET);

	CASE("hold: a hold is refused when no call is up");
	fx_reset();
	CHECK(!call_hold(0, 0));
	CHECK(!call_resume(0, 0));
	CHECK(!call_hangup(0, 0));
}

/* ----------------------------------------------- the RFC 2543 hold sequence */

/*
 * The hold sequence of a field softswitch in a three-way call. It signals
 * hold with c=IN IP4 0.0.0.0 and no direction attribute, and answers a hold
 * offer with
 * the formats 18 0 8. It re-INVITEs the holder about 40 ms later, answers the
 * resume offer with the same stale hold description, then sends an INVITE
 * with no SDP.
 *
 * Every verdict below is the RFC 3264 one, and the answer order decides the
 * codec (RFC 3264 6.1). So 18 0 8 moves a leg that offered PCMU first to
 * G.729, and an answer with no telephone-event leaves the digits inband. No
 * code of this package changes for the sequence; these cases hold it.
 */
static void test_legacy_hold(void)
{
	struct req_info ri;

	fx_reset();
	uac_up(NULL);

	CASE("RFC 2543 hold: our hold offer answered c=0.0.0.0 with no direction is taken");
	CHECK(call_hold(0, 0));
	CHECK(last_req(&ri));
	CHECK(has(ri.body, "a=sendonly"));
	reply(200, NULL, NULL, sdp_body("0.0.0.0", 20000, "18 0 8", NULL));
	CHECK_UINT(ev.reinvite_done, 1);
	CHECK(ev.reinvite_ok);

	CASE("RFC 2543 hold: that answer is a remote hold and the stream goes inactive");
	CHECK_UINT(ev.hold_changed, 1);
	CHECK(ev.held);
	CHECK_UINT(med.last.direction, HAL_MEDIA_INACTIVE);

	CASE("RFC 2543 hold: and it moves the leg to G.729 whatever our offer listed first");
	CHECK(has(ri.body, "m=audio 10000 RTP/AVP 0 8 18 101"));
	CHECK_UINT(med.last.codec, HAL_CODEC_G729);

	CASE("RFC 2543 hold: with no telephone-event in it the digits fall back to inband");
	CHECK_UINT(med.last.dtmf_mode, HAL_DTMF_INBAND);

	CASE("RFC 2543 hold: a peer c=0.0.0.0 re-INVITE on the held leg is answered 200");
	rx(uac_in_dialog("INVITE", "peer-tag", 1, NULL,
			 sdp_body("0.0.0.0", 20000, "18 0 8", NULL)));
	CHECK(has(last_text(), "SIP/2.0 200"));

	CASE("RFC 2543 hold: that answer reads a=inactive at our own address, never 0.0.0.0");
	CHECK(has(last_text(), "a=inactive"));
	CHECK(has(last_text(), "c=IN IP4 " T_HOST));
	CHECK(!has(last_text(), "c=IN IP4 0.0.0.0"));
	rx(uac_in_dialog("ACK", "peer-tag", 1, NULL, NULL));

	CASE("RFC 2543 hold: a resume answered with the stale hold SDP keeps the leg");
	CHECK(call_resume(0, 0));
	reply(200, NULL, NULL, sdp_body("0.0.0.0", 20000, "18 0 8", NULL));
	CHECK(find_send("BYE") < 0);
	CHECK_UINT(ev.failed, 0);

	CASE("RFC 2543 hold: the operation is reported ok with the remote hold noted");
	CHECK_UINT(ev.reinvite_done, 2);
	CHECK(ev.reinvite_ok);
	CHECK_UINT(ev.hold_changed, 1);
	CHECK(ev.held);
	CHECK_UINT(med.last.direction, HAL_MEDIA_RECVONLY);

	CASE("RFC 2543 hold: the offerless INVITE behind it puts our offer in the 200");
	rx(uac_in_dialog("INVITE", "peer-tag", 2, NULL, NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(has(last_text(), "m=audio 10000 RTP/AVP 0 8 18 101"));

	CASE("RFC 2543 hold: that offer carries the real address and the whole codec list");
	CHECK(has(last_text(), "c=IN IP4 " T_HOST));
	CHECK(has(last_text(), "a=rtpmap:18 G729/8000"));
	CHECK(has(last_text(), "a=rtpmap:101 telephone-event/8000"));

	CASE("RFC 2543 hold: and it carries no direction attribute at all");
	CHECK(!has(last_text(), "c=IN IP4 0.0.0.0"));
	CHECK(!has(last_text(), "a=sendonly"));
	CHECK(!has(last_text(), "a=inactive"));
	CHECK(!has(last_text(), "a=recvonly"));

	CASE("RFC 2543 hold: the answer in the ACK programs the relay address sendrecv");
	rx(uac_in_dialog("ACK", "peer-tag", 2, NULL,
			 sdp_body("198.51.100.198", 20002, "0 101", "a=sendrecv\r\n")));
	CHECK_UINT(med.last.direction, HAL_MEDIA_SENDRECV);
	CHECK_UINT(med.last.remote.port, 20002);
	CHECK_UINT(med.last.remote.addr[0], 198);

	CASE("RFC 2543 hold: PCMU comes back and the digits return to "
	     "RFC 2833");
	CHECK_UINT(med.last.codec, HAL_CODEC_PCMU);
	CHECK_UINT(med.last.dtmf_mode, HAL_DTMF_RFC2833);
	CHECK_UINT(ev.hold_changed, 2);
	CHECK(!ev.held);

	CASE("RFC 2543 hold: with no G.729 of ours the 18 0 8 answer selects PCMU");
	fx_reset();
	codec_sections_set(TR104_PCMU, 0);
	calls_reload(&cfg);
	uac_up(NULL);
	CHECK(call_hold(0, 0));
	CHECK(last_req(&ri));
	CHECK(!has(ri.body, "a=rtpmap:18 G729/8000"));
	reply(200, NULL, NULL, sdp_body("0.0.0.0", 20000, "18 0 8", NULL));
	CHECK_UINT(med.last.codec, HAL_CODEC_PCMU);
	cfg_default();
	calls_reload(&cfg);

	CASE("RFC 2543 hold: an offerless INVITE on a locally held leg offers inactive");
	fx_reset();
	uac_up(NULL);
	CHECK(call_hold(0, 0));
	reply(200, NULL, NULL, sdp_body("0.0.0.0", 20000, "0", NULL));
	rx(uac_in_dialog("INVITE", "peer-tag", 1, NULL, NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(has(last_text(), "a=inactive"));

	CASE("RFC 2543 hold: and sendonly when the peer is not holding as "
	     "well");
	fx_reset();
	uac_up(NULL);
	CHECK(call_hold(0, 0));
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "0", "a=recvonly\r\n"));
	rx(uac_in_dialog("INVITE", "peer-tag", 1, NULL, NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(has(last_text(), "a=sendonly"));
}

/* -------------------------------------- our re-INVITE inside a peer dialog */

/*
 * Every hold and resume of an inbound call places a re-INVITE inside a
 * dialog that the peer created, and its 2xx needs an ACK. Without one, the
 * peer retransmits its 200 for 32 seconds and answers the next re-INVITE
 * 500, so a swap or a reconnect behind it fails.
 */
static void test_reinvite_out_uas(void)
{
	struct req_info inv, ack, ack2;
	unsigned int ridx, configs;
	char first[WIRE_MAX];
	int ackidx;

	fx_reset();
	uas_up("call-ua1");

	CASE("uas hold: our re-INVITE takes the first local CSeq of the peer dialog");
	CHECK(call_hold(0, 0));
	CHECK(last_req(&inv));
	CHECK(!strcmp(inv.method, "INVITE"));
	CHECK(has(inv.body, "a=sendonly"));
	CHECK_UINT(inv.cseq, 1);
	ridx = fk.sends - 1;

	CASE("uas hold: the 2xx leaves an ACK on the wire and no complaint behind");
	reply_at(ridx, 200, NULL, NULL, sdp_body(T_PEER, 20000, "0", "a=recvonly\r\n"));
	CHECK_UINT(count_send("ACK"), 1);
	CHECK_UINT(tr.ack_stall_lines, 0);
	ackidx = find_send("ACK");
	CHECK(ackidx > 0);
	CHECK(req_info_at((unsigned int)ackidx, &ack));
	CHECK(!strcmp(ack.method, "ACK"));

	CASE("uas hold: the ACK carries the dialog identity in the UAS orientation");
	CHECK(!strcmp(ack.call_id, inv.call_id));
	CHECK(!strcmp(ack.from_tag, inv.from_tag));
	CHECK(!strcmp(ack.to_tag, inv.to_tag));
	CHECK(!strcmp(ack.ruri, inv.ruri));
	CHECK_UINT(ack.cseq, inv.cseq);
	CHECK(has(send_text((unsigned int)ackidx), "Route: <sip:proxy.example.net;lr>"));

	CASE("uas hold: the operation completes and the stream turns send only");
	CHECK_UINT(ev.reinvite_done, 1);
	CHECK(ev.reinvite_ok);
	CHECK_UINT(med.last.direction, HAL_MEDIA_SENDONLY);
	CHECK_UINT(ev.failed, 0);

	CASE("uas hold: a retransmitted 2xx is answered with the identical bytes");
	configs = med.config_calls;
	snprintf(first, sizeof(first), "%s", send_text((unsigned int)ackidx));
	CHECK(first[0] != 0);
	reply_at(ridx, 200, NULL, NULL, sdp_body(T_PEER, 20000, "0", "a=recvonly\r\n"));
	CHECK_UINT(count_send("ACK"), 2);
	CHECK(!strcmp(send_text(fk.sends - 1), first));

	CASE("uas hold: the retransmission moves nothing else on the leg");
	CHECK_UINT(ev.reinvite_done, 1);
	CHECK_UINT(med.config_calls, configs);
	CHECK_UINT(ev.failed, 0);
	CHECK(find_send("BYE") < 0);
	CHECK(slot_is(1));

	CASE("uas resume: the second exchange is acknowledged the same way");
	CHECK(call_resume(0, 0));
	CHECK(last_req(&inv));
	CHECK_UINT(inv.cseq, 2);
	ridx = fk.sends - 1;
	reply_at(ridx, 200, NULL, NULL, sdp_body(T_PEER, 20000, "0", "a=sendrecv\r\n"));
	CHECK_UINT(count_send("ACK"), 3);
	CHECK(req_info_at((unsigned int)find_send("ACK"), &ack2));
	CHECK_UINT(ack2.cseq, 2);
	CHECK(!strcmp(ack2.from_tag, ack.from_tag));
	CHECK(!strcmp(ack2.to_tag, ack.to_tag));
	CHECK_UINT(tr.ack_stall_lines, 0);
	CHECK_UINT(ev.reinvite_done, 2);
	CHECK(ev.reinvite_ok);
	CHECK_UINT(med.last.direction, HAL_MEDIA_SENDRECV);

	CASE("uas hold: the peer BYE still ends the call after both exchanges");
	rx(uas_in_dialog("BYE", 4, NULL, NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(ev.remote_bye, 1);
	CHECK_UINT(ev.failed, 0);
}

static void test_admission(void)
{
	unsigned int slot = 99;

	CASE("admission: an exhausted RTP range fails the second call congested");
	fx_reset();
	cfg.channel[0].rtp_port_min = 10000;
	cfg.channel[0].rtp_port_max = 10001;
	CHECK(call_dial(0, 0, "1002", true));
	CHECK(!call_dial(0, 1, "1003", true));
	CHECK_UINT(count_send("INVITE"), 1);
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.cls, CALL_FAIL_CONGESTION);

	CASE("admission: an inbound call with no port left is answered 486");
	rx(invite_text("call-a1", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 486"));
	CHECK_UINT(ev.incoming, 0);
	cfg.channel[0].rtp_port_min = 10000;
	cfg.channel[0].rtp_port_max = 10030;

	CASE("admission: two calls fill the line");
	fx_reset();
	rx(invite_text("call-a2", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK_UINT(ev.incoming, 1);
	CHECK(call_ring(0, 0));
	rx(invite_text("call-a3", NULL, sdp_body(T_PEER, 20002, "0", NULL)));
	CHECK_UINT(ev.incoming, 2);
	CHECK_UINT(ev.slot, 1);
	CHECK(!call_slot_free(0, &slot));

	CASE("admission: a third INVITE is answered 486 before any dialog");
	rx(invite_text("call-a4", NULL, sdp_body(T_PEER, 20004, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 486"));
	CHECK_UINT(ev.incoming, 2);
	CHECK_UINT(sip_dialog_count(&dlgs), 1);

	CASE("admission: the two legs allocated different RTP ports");
	CHECK(call_answer(0, 0));
	CHECK_UINT(med.last.local.port, 10000);
	CHECK(call_answer(0, 1));
	CHECK_UINT(med.last.local.port, 10002);

	CASE("admission: the second leg drives the second stream");
	CHECK_UINT(med.last_stream, 1);

	CASE("admission: a released leg gives its port back");
	fx_reset();
	cfg.channel[0].rtp_port_max = 10001;	/* exactly one port in the pool */
	CHECK(call_dial(0, 0, "1002", true));
	CHECK(call_hangup(0, 0));
	fake_advance(40000);
	CHECK(call_dial(0, 0, "1003", true));
	CHECK(has(last_text(), "m=audio 10000"));
	cfg.channel[0].rtp_port_max = 10030;

	CASE("admission: a draining leg does not block the slot");
	fx_reset();
	uac_up(NULL);
	CHECK(call_hangup(0, 0));
	CHECK(slot_is(1));
	CHECK(call_dial(0, 1, "1004", true));

	/*
	 * The bring-up window and the dead DSP. The shell wires this layer at
	 * start-up, before the DSP has finished starting, and a bring-up that
	 * failed never finishes at all. The subscriber is not busy in either
	 * state, so the answer says the service is unavailable and names when
	 * to come back.
	 */
	CASE("admission: an INVITE while the DSP is still starting is answered 503");
	fx_reset();
	fk.hal_state = UVOICE_HAL_STARTING;
	rx(invite_text("call-hal1", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 503"));
	CHECK_UINT(ev.incoming, 0);
	CHECK_UINT(sip_dialog_count(&dlgs), 0);
	CHECK(slot_is(0));

	CASE("admission: the 503 carries the Retry-After the caller comes back on");
	CHECK(has(last_text(), "Retry-After: 5"));

	CASE("admission: a bring-up that failed answers 503 as well");
	fx_reset();
	fk.hal_state = UVOICE_HAL_FAILED;
	rx(invite_text("call-hal2", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 503"));
	CHECK(has(last_text(), "Retry-After: 5"));
	CHECK_UINT(ev.incoming, 0);
	CHECK_UINT(sip_dialog_count(&dlgs), 0);

	CASE("admission: a ready DSP takes the same call");
	fx_reset();
	rx(invite_text("call-hal3", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK_UINT(ev.incoming, 1);
	CHECK(find_send("SIP/2.0 503") < 0);

	/*
	 * The order inside the admission tail. Both slots are taken and the
	 * hardware cannot carry a call: the line is out of service, not busy,
	 * so the hardware answer is the one that goes out. account.c's request
	 * intake answers a full line 486 on its own, so the case drives the
	 * tail directly, the way the account drain cases do.
	 */
	CASE("admission: a line whose DSP is not ready answers 503 with both slots full");
	fx_reset();
	rx(invite_text("call-hal4", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(call_ring(0, 0));
	rx(invite_text("call-hal5", NULL, sdp_body(T_PEER, 20002, "0", NULL)));
	CHECK_UINT(ev.incoming, 2);
	CHECK(!call_slot_free(0, &slot));
	fk.hal_state = UVOICE_HAL_STARTING;
	rx_direct(0, invite_text("call-hal6", NULL, sdp_body(T_PEER, 20004, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 503"));
	CHECK_UINT(ev.incoming, 2);

	/*
	 * The line the shell holds out of service. Its machine ignores the
	 * incoming call event, so a leg allocated for it would ring nothing
	 * and the caller would hear ringback on a silent port. The subscriber
	 * is not busy, so the answer is the 480 an account being torn down
	 * gets, and it goes out before any leg exists.
	 */
	CASE("admission: an INVITE to an out of service line is answered 480");
	fx_reset();
	fk.line_oos[0] = true;
	rx(invite_text("call-oos1", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 480"));
	CHECK_UINT(ev.incoming, 0);
	CHECK_UINT(sip_dialog_count(&dlgs), 0);
	CHECK(slot_is(0));

	CASE("admission: the same line in service takes the call");
	fx_reset();
	rx(invite_text("call-oos2", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK_UINT(ev.incoming, 1);
	CHECK(find_send("SIP/2.0 480") < 0);

	/*
	 * A new call needs a live registration. The subscriber is not busy and
	 * the line is not out of service: the callee is not logged in, which is
	 * the 480 of RFC 3261 21.4.18.
	 */
	CASE("admission: an INVITE to a line with no live registration is answered 480");
	fx_reset();
	fk.registered = false;
	rx(invite_text("call-reg1", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 480"));
	CHECK_UINT(ev.incoming, 0);
	CHECK_UINT(sip_dialog_count(&dlgs), 0);
	CHECK(slot_is(0));

	CASE("admission: the call layer driven directly answers the same 480");
	fx_reset();
	fk.registered = false;
	rx_direct(0, invite_text("call-reg2", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 480"));
	CHECK_UINT(ev.incoming, 0);
	CHECK(slot_is(0));

	CASE("admission: no live registration answers 480 in front of the 503 of a DSP that is not ready");
	fx_reset();
	fk.registered = false;
	fk.hal_state = UVOICE_HAL_STARTING;
	rx_direct(0, invite_text("call-reg3", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 480"));
	CHECK(find_send("SIP/2.0 503") < 0);

	CASE("admission: with both slots busy, no live registration answers 480, not 486");
	fx_reset();
	rx(invite_text("call-reg4", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(call_ring(0, 0));
	rx(invite_text("call-reg5", NULL, sdp_body(T_PEER, 20002, "0", NULL)));
	CHECK_UINT(ev.incoming, 2);
	fk.registered = false;
	rx(invite_text("call-reg6", NULL, sdp_body(T_PEER, 20004, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 480"));
	CHECK(find_send("SIP/2.0 486") < 0);
	CHECK_UINT(ev.incoming, 2);

	CASE("admission: a line with a live registration still rings");
	fx_reset();
	rx(invite_text("call-reg7", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK_UINT(ev.incoming, 1);
	CHECK(find_send("SIP/2.0 480") < 0);

	/*
	 * The two refusals stay distinct: an account that is unregistering is
	 * going away for good, and the caller is told that first whatever the
	 * DSP is doing.
	 */
	CASE("admission: an account going away answers 480, not 503");
	fx_reset();
	fk.active = false;
	fk.hal_state = UVOICE_HAL_STARTING;
	rx_direct(0, invite_text("call-hal7", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 480"));
	CHECK_UINT(ev.incoming, 0);

	/*
	 * The ghost ring. An offer whose audio address the media layer can
	 * never program has one outcome only, and the subscriber does not
	 * decide it: sdp.c refuses the same address at the answer. A leg that
	 * rang would refuse at the lift and end, so the refusal comes before
	 * the ring.
	 */
	CASE("ghost ring: an INVITE whose c= names a host is answered 488");
	fx_reset();
	rx(invite_text("call-gr1", NULL, sdp_body("sip.example.com", 20000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 488"));

	/* the once-only report below is what catches a leaked leg: a leg that
	 * was never released still reads slot-free through the FREE-state scan */
	CASE("ghost ring: nothing rang and the slot reads free");
	CHECK_UINT(ev.incoming, 0);
	CHECK_UINT(sip_dialog_count(&dlgs), 0);
	CHECK(slot_is(0));

	/* the same report the body we cannot parse makes, for the same reason:
	 * a leg existed for the length of the refusal */
	CASE("ghost ring: the refusal reports the inbound leg exactly once");
	CHECK_UINT(ev.ended, 1);
	CHECK(ev.end.inbound);
	CHECK(!ev.end.answered);
	CHECK(!ev.end.media_ran);

	CASE("ghost ring: the RFC 4566 multicast TTL notation is refused as well");
	fx_reset();
	rx(invite_text("call-gr2", NULL, sdp_body("224.2.1.1/127", 20000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 488"));
	CHECK_UINT(ev.incoming, 0);

	/* a body that parses and names no address at all: the answer refused it
	 * as SDP_BAD after the ring, and it is the same refusal in front of it */
	CASE("ghost ring: an offer with no c= line is refused before the ring too");
	fx_reset();
	rx(invite_text("call-gr15", NULL,
		       "v=0\r\no=- 1 1 IN IP4 198.51.100.7\r\ns=-\r\nt=0 0\r\n"
		       "m=audio 20000 RTP/AVP 0\r\n"));
	CHECK(has(last_text(), "SIP/2.0 488"));
	CHECK_UINT(ev.incoming, 0);
	CHECK(slot_is(0));

	CASE("ghost ring: the literal control rings and answers nothing");
	fx_reset();
	rx(invite_text("call-gr3", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK_UINT(ev.incoming, 1);
	CHECK(find_send("SIP/2.0 488") < 0);
	CHECK(call_ring(0, 0));
	CHECK(has(last_text(), "SIP/2.0 180"));

	CASE("ghost ring: the c=0.0.0.0 hold form is a literal and still rings");
	fx_reset();
	rx(invite_text("call-gr4", NULL, sdp_body("0.0.0.0", 20000, "0", NULL)));
	CHECK_UINT(ev.incoming, 1);
	CHECK(find_send("SIP/2.0 488") < 0);

	CASE("ghost ring: an offerless INVITE has no offer to refuse and rings");
	fx_reset();
	rx(invite_text("call-gr5", NULL, NULL));
	CHECK_UINT(ev.incoming, 1);
	CHECK(find_send("SIP/2.0 488") < 0);

	/*
	 * The refused offer leaves nothing behind. offer_valid marks an offer
	 * this leg will answer, so the refusal never sets it: the next call on
	 * the same slot, an offerless one, must offer from our own description
	 * instead of answering a description the previous caller sent.
	 */
	CASE("ghost ring: an offerless INVITE after a refusal is not answered from a stale offer");
	fx_reset();
	rx(invite_text("call-gr12", NULL, sdp_body("sip.example.com", 20000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 488"));
	rx(invite_text("call-gr13", NULL, NULL));
	CHECK_UINT(ev.incoming, 1);
	CHECK(call_answer(0, 0));
	CHECK(has(last_text(), "SIP/2.0 200"));
	/* our full offer, not an answer built from the refused description (the
	 * RTP port is the second one of the pool: the first is still cooling) */
	CHECK(has(last_text(), "RTP/AVP 0 8 18 101"));
	CHECK(!has(last_text(), "sip.example.com"));

	/*
	 * The precedence, pinned. The new refusal is the last of the inbound
	 * gates: the 480 and the 503 answer for the account and for the
	 * hardware, which the caller's offer cannot change, and the 486 answers
	 * for a line that is busy, which never reads an offer at all.
	 */
	CASE("ghost ring: an account going away answers 480, not the new 488");
	fx_reset();
	fk.active = false;
	rx_direct(0, invite_text("call-gr6", NULL,
				 sdp_body("sip.example.com", 20000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 480"));
	CHECK_UINT(ev.ended, 0);

	CASE("ghost ring: a DSP that is not ready answers 503, not the new 488");
	fx_reset();
	fk.hal_state = UVOICE_HAL_STARTING;
	rx(invite_text("call-gr7", NULL, sdp_body("sip.example.com", 20000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 503"));
	CHECK_UINT(ev.ended, 0);

	/*
	 * The RFC 4028 floor keeps its place too. It is one stateless rule read
	 * in front of the leg, shared with rx_reinvite's
	 * sess_min_ok, and a caller that is wrong twice pays one extra round
	 * trip: it asks for a session we accept, and meets the 488 then.
	 */
	CASE("ghost ring: a Session-Expires below the floor answers 422, not the new 488");
	fx_reset();
	rx(invite_text("call-gr14", "Session-Expires: 60\r\n",
		       sdp_body("sip.example.com", 20000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 422"));
	CHECK(find_send("SIP/2.0 488") < 0);
	CHECK_UINT(ev.incoming, 0);
	CHECK_UINT(ev.ended, 0);
	CHECK(slot_is(0));

	CASE("ghost ring: a full line answers 486, the slot check reads no offer");
	fx_reset();
	rx(invite_text("call-gr8", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(call_ring(0, 0));
	rx(invite_text("call-gr9", NULL, sdp_body(T_PEER, 20002, "0", NULL)));
	CHECK_UINT(ev.incoming, 2);
	rx_direct(0, invite_text("call-gr10", NULL,
				 sdp_body("sip.example.com", 20004, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 486"));
	CHECK_UINT(ev.incoming, 2);
	CHECK_UINT(ev.ended, 0);

	/* the media take runs in front of the offer parse, so a line with no
	 * RTP port left answers for the port it does not have */
	CASE("ghost ring: a line with no RTP port answers 486, offer unread");
	fx_reset();
	cfg.channel[0].rtp_port_min = 10000;
	cfg.channel[0].rtp_port_max = 10001;
	CHECK(call_dial(0, 0, "1002", true));
	rx(invite_text("call-gr11", NULL, sdp_body("sip.example.com", 20000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 486"));
	CHECK_UINT(ev.incoming, 0);
	cfg.channel[0].rtp_port_max = 10030;

	/*
	 * RFC 3261 8.1.1.3: a From with no tag names the remote half of no
	 * dialog, so sip_dialog_uas_create() refuses it. The 400 at admission
	 * comes before the ring, which would end in a 500 at the lift.
	 */
	CASE("admission: an INVITE whose From carries no tag is answered 400");
	fx_reset();
	rx(invite_text_untagged("call-ft1", sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 400"));
	CHECK_UINT(fk.sends, 1);

	CASE("admission: the 400 leaves no leg, no ring and no record");
	CHECK_UINT(ev.incoming, 0);
	CHECK_UINT(ev.ended, 0);
	CHECK_UINT(sip_dialog_count(&dlgs), 0);
	CHECK(slot_is(0));
	CHECK(find_send("SIP/2.0 180") < 0);

	CASE("admission: the same INVITE with a tag rings as before");
	fx_reset();
	rx(invite_text("call-ft2", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK_UINT(ev.incoming, 1);
	CHECK(call_ring(0, 0));
	CHECK(has(last_text(), "SIP/2.0 180"));
	CHECK_UINT(sip_dialog_count(&dlgs), 1);
}

/* --------------------------------------------------------- the status dump */

enum {
	SV_SLOT,
	SV_UI_BUSY,
	SV_CODEC,
	SV_PTIME,
	SV_REMOTE,
	SV_NAME,
	SV_CID_PRIVATE,
	SV_FAX,
	SV_FAX_KIND,
	SV_SRTP,
	SV_STATE,
	SV_UI_SLOT,
	SV_SHADOW,
	__SV_MAX,
};

static const struct blobmsg_policy sv_policy[__SV_MAX] = {
	[SV_SLOT] = { "slot", BLOBMSG_TYPE_INT32 },
	[SV_UI_BUSY] = { "ui_busy", BLOBMSG_TYPE_BOOL },
	[SV_CODEC] = { "codec", BLOBMSG_TYPE_STRING },
	[SV_PTIME] = { "ptime", BLOBMSG_TYPE_INT32 },
	[SV_REMOTE] = { "remote", BLOBMSG_TYPE_STRING },
	[SV_NAME] = { "name", BLOBMSG_TYPE_STRING },
	[SV_CID_PRIVATE] = { "cid_private", BLOBMSG_TYPE_BOOL },
	[SV_FAX] = { "fax", BLOBMSG_TYPE_STRING },
	[SV_FAX_KIND] = { "fax_kind", BLOBMSG_TYPE_STRING },
	[SV_SRTP] = { "srtp", BLOBMSG_TYPE_STRING },
	[SV_STATE] = { "state", BLOBMSG_TYPE_STRING },
	[SV_UI_SLOT] = { "ui_slot", BLOBMSG_TYPE_INT32 },
	[SV_SHADOW] = { "shadow", BLOBMSG_TYPE_BOOL },
};

/* One leg of the "calls" array, by slot, so a case asserts exact dump keys. */
static bool slot_view_get(unsigned int line, unsigned int slot, struct slot_view *v)
{
	struct blob_attr *cur, *entry;
	struct blob_buf b;
	size_t rem, rem2;

	memset(v, 0, sizeof(*v));
	memset(&b, 0, sizeof(b));
	blob_buf_init(&b, 0);
	calls_status_dump(&b, line);

	blob_for_each_attr(cur, b.head, rem) {
		if (strcmp(blobmsg_name(cur), "calls"))
			continue;

		blobmsg_for_each_attr(entry, cur, rem2) {
			struct blob_attr *tb[__SV_MAX] = {};

			blobmsg_parse(sv_policy, __SV_MAX, tb, blobmsg_data(entry),
				      blobmsg_data_len(entry));
			if (!tb[SV_SLOT] || blobmsg_get_u32(tb[SV_SLOT]) != slot)
				continue;

			v->found = true;
			v->has_ui_slot = tb[SV_UI_SLOT] != NULL;
			if (tb[SV_UI_SLOT])
				v->ui_slot = blobmsg_get_u32(tb[SV_UI_SLOT]);
			v->has_shadow = tb[SV_SHADOW] != NULL;
			if (tb[SV_SHADOW])
				v->shadow = blobmsg_get_bool(tb[SV_SHADOW]);
			if (tb[SV_UI_BUSY])
				v->ui_busy = blobmsg_get_bool(tb[SV_UI_BUSY]);
			if (tb[SV_CODEC])
				snprintf(v->codec, sizeof(v->codec), "%s",
					 blobmsg_get_string(tb[SV_CODEC]));
			if (tb[SV_PTIME]) {
				v->has_ptime = true;
				v->ptime = blobmsg_get_u32(tb[SV_PTIME]);
			}
			if (tb[SV_REMOTE])
				snprintf(v->remote, sizeof(v->remote), "%s",
					 blobmsg_get_string(tb[SV_REMOTE]));
			if (tb[SV_NAME])
				snprintf(v->name, sizeof(v->name), "%s",
					 blobmsg_get_string(tb[SV_NAME]));
			if (tb[SV_CID_PRIVATE])
				v->cid_private = blobmsg_get_bool(tb[SV_CID_PRIVATE]);
			if (tb[SV_FAX])
				snprintf(v->fax, sizeof(v->fax), "%s",
					 blobmsg_get_string(tb[SV_FAX]));
			if (tb[SV_FAX_KIND])
				snprintf(v->fax_kind, sizeof(v->fax_kind), "%s",
					 blobmsg_get_string(tb[SV_FAX_KIND]));
			if (tb[SV_SRTP])
				snprintf(v->srtp, sizeof(v->srtp), "%s",
					 blobmsg_get_string(tb[SV_SRTP]));
			if (tb[SV_STATE])
				snprintf(v->state, sizeof(v->state), "%s",
					 blobmsg_get_string(tb[SV_STATE]));
		}
	}

	blob_buf_free(&b);

	return v->found;
}

enum {
	RV_BUSY,
	RV_UNAVAILABLE,
	RV_OTHER,
	RV_LAST_CODE,
	RV_LAST_AGE_MS,
	__RV_MAX,
};

static const struct blobmsg_policy rv_policy[__RV_MAX] = {
	[RV_BUSY] = { "busy", BLOBMSG_TYPE_INT32 },
	[RV_UNAVAILABLE] = { "unavailable", BLOBMSG_TYPE_INT32 },
	[RV_OTHER] = { "other", BLOBMSG_TYPE_INT32 },
	[RV_LAST_CODE] = { "last_code", BLOBMSG_TYPE_INT32 },
	[RV_LAST_AGE_MS] = { "last_age_ms", BLOBMSG_TYPE_INT32 },
};

struct refused_view {
	unsigned int busy;
	unsigned int unavailable;
	unsigned int other;
	unsigned int last_code;
	unsigned int last_age_ms;
	/* every key of the table, so a key the schema does not know fails a
	 * case here and not the CLI schema leg */
	unsigned int keys;
	bool found;
};

/*
 * The refusal table of one line. It is absent until the line refused a call,
 * so the answer of this reader is itself what a case asserts first.
 */
static bool refused_view_get(unsigned int line, struct refused_view *v)
{
	struct blob_attr *tb[__RV_MAX] = {};
	struct blob_attr *cur, *member;
	struct blob_buf b;
	size_t rem, rem2;

	memset(v, 0, sizeof(*v));
	memset(&b, 0, sizeof(b));
	blob_buf_init(&b, 0);
	calls_status_dump(&b, line);

	blob_for_each_attr(cur, b.head, rem) {
		if (strcmp(blobmsg_name(cur), "refused"))
			continue;

		v->found = true;
		blobmsg_for_each_attr(member, cur, rem2)
			v->keys++;

		blobmsg_parse(rv_policy, __RV_MAX, tb, blobmsg_data(cur),
			      blobmsg_data_len(cur));
		if (tb[RV_BUSY])
			v->busy = blobmsg_get_u32(tb[RV_BUSY]);
		if (tb[RV_UNAVAILABLE])
			v->unavailable = blobmsg_get_u32(tb[RV_UNAVAILABLE]);
		if (tb[RV_OTHER])
			v->other = blobmsg_get_u32(tb[RV_OTHER]);
		if (tb[RV_LAST_CODE])
			v->last_code = blobmsg_get_u32(tb[RV_LAST_CODE]);
		if (tb[RV_LAST_AGE_MS])
			v->last_age_ms = blobmsg_get_u32(tb[RV_LAST_AGE_MS]);
	}

	blob_buf_free(&b);

	return v->found;
}

/*
 * The refusal counters of a line. A refused call rings nothing.
 * A busy rejection of an offered call still closes a record: the call counts in
 * in_failed, and the record reads fail busy. Without the
 * counters, the field cannot tell a line that turns every call away from a
 * line that nobody calls.
 */
static void test_refused(void)
{
	struct refused_view rv;
	uint32_t txn;

	CASE("refused: a busy rejection counts under busy and names 486");
	fx_reset();
	rx(invite_text("call-rf1", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK_UINT(ev.incoming, 1);
	CHECK(call_reject(0, 0, 486));
	CHECK(has(last_text(), "SIP/2.0 486"));
	CHECK(refused_view_get(0, &rv));
	CHECK_UINT(rv.busy, 1);
	CHECK_UINT(rv.unavailable, 0);
	CHECK_UINT(rv.other, 0);
	CHECK_UINT(rv.last_code, 486);
	CHECK_UINT(rv.keys, 5);

	/* the two facts cdr_call_ended() reads to move in_failed; the counter
	 * itself belongs to test_cdr.c */
	CASE("refused: a busy rejection counts once under in_failed");
	CHECK_UINT(ev.ended, 1);
	CHECK(ev.end.inbound);
	CHECK(ev.end.failed);
	CHECK_UINT(ev.end.cls, CALL_FAIL_BUSY);

	CASE("refused: the machine sees no failure event for a call it "
	     "rejected itself");
	CHECK_UINT(ev.failed, 0);

	/* the call layer counts its own answer; the one account.c gives in
	 * front of it is uncounted, as its 486 for a full line is */
	CASE("refused: a 480 for no live registration counts under unavailable");
	fx_reset();
	fk.registered = false;
	rx_direct(0, invite_text("call-rf-reg", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 480"));
	CHECK(refused_view_get(0, &rv));
	CHECK_UINT(rv.unavailable, 1);
	CHECK_UINT(rv.last_code, 480);

	CASE("refused: a 480 before any leg counts under unavailable");
	fx_reset();
	fk.line_oos[0] = true;
	rx(invite_text("call-rf2", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 480"));
	CHECK_UINT(ev.incoming, 0);
	CHECK(refused_view_get(0, &rv));
	CHECK_UINT(rv.unavailable, 1);
	CHECK_UINT(rv.busy, 0);
	CHECK_UINT(rv.other, 0);
	CHECK_UINT(rv.last_code, 480);

	CASE("refused: a 400, a 422 and a 488 all count under other");
	fx_reset();
	rx(invite_text_untagged("call-rf3",
				sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 400"));
	cfg.channel[0].session_min_interval = 900;
	rx(invite_text("call-rf4", "Session-Expires: 120;refresher=uac\r\n",
		       sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 422"));
	cfg.channel[0].session_min_interval = 0;
	rx(invite_text("call-rf5", NULL, "this is not sdp\r\n"));
	CHECK(has(last_text(), "SIP/2.0 488"));
	CHECK(refused_view_get(0, &rv));
	CHECK_UINT(rv.other, 3);
	CHECK_UINT(rv.busy, 0);
	CHECK_UINT(rv.unavailable, 0);

	CASE("refused: the last refusal carries its code and an age that "
	     "grows");
	CHECK(refused_view_get(0, &rv));
	CHECK_UINT(rv.last_code, 488);
	CHECK_UINT(rv.last_age_ms, 0);
	fake_advance(1500);
	CHECK(refused_view_get(0, &rv));
	CHECK_UINT(rv.last_code, 488);
	CHECK_UINT(rv.last_age_ms, 1500);

	CASE("refused: a DSP that is not ready counts its 503 under "
	     "unavailable");
	fx_reset();
	fk.hal_state = UVOICE_HAL_STARTING;
	rx(invite_text("call-rf7", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 503"));
	CHECK(refused_view_get(0, &rv));
	CHECK_UINT(rv.unavailable, 1);
	CHECK_UINT(rv.busy, 0);
	CHECK_UINT(rv.other, 0);
	CHECK_UINT(rv.last_code, 503);

	/* the fixture answers a full line 486 itself, the way account.c does,
	 * so the capacity rule of call.c is driven directly */
	CASE("refused: a line with both slots taken counts its 486 under busy");
	fx_reset();
	rx(invite_text("call-rf8", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(call_ring(0, 0));
	rx(invite_text("call-rf9", NULL, sdp_body(T_PEER, 20002, "0", NULL)));
	CHECK_UINT(ev.incoming, 2);
	rx_direct(0, invite_text("call-rf10", NULL,
				 sdp_body(T_PEER, 20004, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 486"));
	CHECK(refused_view_get(0, &rv));
	CHECK_UINT(rv.busy, 1);
	CHECK_UINT(rv.unavailable, 0);
	CHECK_UINT(rv.other, 0);
	CHECK_UINT(rv.last_code, 486);

	CASE("refused: a 180 that cannot be built counts its 500 under other");
	fx_reset();
	dialog_pool_fill();
	rx(invite_text("call-rf11", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(!call_ring(0, 0));
	CHECK(has(last_text(), "SIP/2.0 500"));
	CHECK(refused_view_get(0, &rv));
	CHECK_UINT(rv.other, 1);
	CHECK_UINT(rv.busy, 0);
	CHECK_UINT(rv.unavailable, 0);
	CHECK_UINT(rv.last_code, 500);

	/* the counters record what a caller was told, so a leg whose server
	 * transaction died refuses without a response and without a count */
	CASE("refused: a refusal that reaches no caller counts nothing");
	fx_reset();
	txn = rx_direct(0, invite_text("call-rf12", NULL,
				       sdp_body(T_PEER, 20000, "0", NULL)));
	sip_txn_abort(sip_txn_get(&mgr, txn));
	CHECK(!call_ring(0, 0));
	CHECK_UINT(fk.sends, 0);
	CHECK(!refused_view_get(0, &rv));

	txn = rx_direct(0, invite_text("call-rf13", NULL,
				       sdp_body(T_PEER, 20000, "0", NULL)));
	sip_txn_abort(sip_txn_get(&mgr, txn));
	CHECK(call_reject(0, 0, 486));
	CHECK_UINT(fk.sends, 0);
	CHECK(!refused_view_get(0, &rv));

	/* the teardown reports the quiet class before it rejects the offered
	 * leg, and the record must name the class the machine was told */
	CASE("refused: a teardown keeps the quiet class the machine was told "
	     "and still counts the 480");
	fx_reset();
	rx(invite_text("call-rf6", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(call_ring(0, 0));
	calls_line_teardown(0);
	CHECK(has(last_text(), "SIP/2.0 480"));
	CHECK_UINT(ev.ended, 1);
	CHECK(ev.end.failed);
	CHECK_UINT(ev.end.cls, CALL_FAIL_QUIET);
	CHECK(refused_view_get(0, &rv));
	CHECK_UINT(rv.unavailable, 1);
	CHECK_UINT(rv.last_code, 480);

	CASE("refused: a line with no refusal emits no table at all");
	CHECK(!refused_view_get(1, &rv));
	fx_reset();
	CHECK(!refused_view_get(0, &rv));
}

/*
 * The Alert-Info dialects and the one ring template vocabulary they fold
 * into. The parse layer names the dialect and keeps the digit or the token as
 * it arrived; call.c maps it, and the line layer rings template 0 as the
 * default. Four dialects share the vocabulary: BroadSoft, Nortel, Sylantro
 * and NZ.
 */
static void alert_hdr_ring_check(const char *hdr, unsigned int want)
{
	fx_reset();
	rx(invite_text("call-alert", hdr, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK_UINT(ev.incoming, 1);
	CHECK_UINT(ev.ring_id, want);
}

static void alert_ring_check(const char *info, unsigned int want)
{
	char hdr[160];

	snprintf(hdr, sizeof(hdr), "Alert-Info: <http://host/x.wav>;info=%s\r\n", info);
	alert_hdr_ring_check(hdr, want);
}

static void alert_url_ring_check(const char *url, unsigned int want)
{
	char hdr[160];

	snprintf(hdr, sizeof(hdr), "Alert-Info: <%s>\r\n", url);
	alert_hdr_ring_check(hdr, want);
}

/* the token that names no ring arrives first and must not end the walk */
static void alert_after_check(const char *info, unsigned int want)
{
	char hdr[200];

	snprintf(hdr, sizeof(hdr),
		 "Alert-Info: <http://host/a.wav>;info=%s, "
		 "<http://host/b.wav>;info=alert-external\r\n", info);
	alert_hdr_ring_check(hdr, want);
}

static void test_alert_info(void)
{
	CASE("sylantro: alert-internal rings template 1");
	alert_ring_check("alert-internal", 1);

	CASE("sylantro: alert-external rings template 2");
	alert_ring_check("alert-external", 2);

	CASE("sylantro: alert-priority rings template 3");
	alert_ring_check("alert-priority", 3);

	CASE("sylantro: alert-emergency rings template 3 as well");
	alert_ring_check("alert-emergency", 3);

	CASE("sylantro: alert-acd rings template 4");
	alert_ring_check("alert-acd", 4);

	CASE("sylantro: alert-group rings template 4");
	alert_ring_check("alert-group", 4);

	CASE("sylantro: every alert-community token rings template 4");
	alert_ring_check("alert-community-1", 4);
	alert_ring_check("alert-community-24", 4);

	CASE("sylantro: the token is matched without case");
	alert_ring_check("Alert-External", 2);

	CASE("sylantro: the token in the URL itself maps the same way");
	alert_url_ring_check("http://host/alert-internal", 1);

	CASE("sylantro: an auto-answer class names no ring pattern");
	alert_ring_check("alert-autoanswer", 0);
	CHECK_UINT(tr.alert_lines, 1);
	CHECK(strstr(tr.alert, "alert pattern ") != NULL);
	CHECK(strstr(tr.alert, "alert-autoanswer") != NULL);

	CASE("sylantro: the bare answer token names none either");
	alert_ring_check("answer", 0);

	CASE("sylantro: the dead visual token names none either");
	alert_ring_check("alert-visual", 0);

	CASE("sylantro: an unknown alert token falls to the default ring");
	alert_ring_check("alert-nosuchthing", 0);

	CASE("sylantro: a token the map only prefixes is not a match");
	alert_ring_check("alert-internally", 0);

	CASE("sylantro: the bare alert- prefix names no dialect at all");
	alert_ring_check("alert-", 0);
	CHECK_UINT(tr.alert_lines, 1);
	CHECK(strstr(tr.alert, "alert info ") != NULL);

	/*
	 * The walk goes past a value whose token names no ring, so a mapped
	 * token behind an auto-answer class still selects the ring.
	 */
	CASE("a token that names no ring does not end the walk");
	alert_after_check("alert-autoanswer", 2);
	alert_after_check("answer", 2);
	alert_after_check("alert-visual", 2);
	alert_after_check("alert-nosuchthing", 2);

	CASE("bellcore: the other dialect still names its own ring");
	alert_url_ring_check("http://127.0.0.1/Bellcore-dr7", 7);

	CASE("bellcore: 8 and 9 have no template and ring the default");
	alert_url_ring_check("http://127.0.0.1/Bellcore-dr8", 0);
	CHECK_UINT(tr.alert_lines, 1);
	CHECK(strstr(tr.alert, "alert pattern ") != NULL);
	CHECK(strstr(tr.alert, "Bellcore-dr8") != NULL);
	alert_url_ring_check("http://127.0.0.1/Bellcore-dr9", 0);

	CASE("bellcore: a configured prefix selects the ring with the same rule");
	fx_reset();
	snprintf(cfg.alert_info_prefix, sizeof(cfg.alert_info_prefix), "%s", "Ring-X");
	calls_reload(&cfg);
	rx(invite_text("call-alert-prefix", "Alert-Info: <http://127.0.0.1/Ring-X4>\r\n",
		       sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK_UINT(ev.ring_id, 4);

	/* the walk re-passes the prefix, so a later value reads the same one */
	CASE("bellcore: a configured prefix reaches the second value as well");
	fx_reset();
	rx(invite_text("call-alert-prefix2",
		       "Alert-Info: <http://host/ring.wav>, <http://127.0.0.1/Ring-X4>\r\n",
		       sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK_UINT(ev.incoming, 1);
	CHECK_UINT(ev.ring_id, 4);
	snprintf(cfg.alert_info_prefix, sizeof(cfg.alert_info_prefix), "%s", "Bellcore-dr");
	calls_reload(&cfg);

	CASE("nortel: standard_ring is the plain ring");
	alert_url_ring_check("http://127.0.0.1/nortel/standard_ring", 1);

	CASE("nortel: dst_ring_1 to dst_ring_3 take the GR-506 numbers 2 to 4");
	alert_url_ring_check("http://127.0.0.1/nortel/dst_ring_1", 2);
	alert_url_ring_check("http://127.0.0.1/nortel/dst_ring_2", 3);
	alert_url_ring_check("http://127.0.0.1/nortel/dst_ring_3", 4);

	CASE("nortel: splash_ring is the splash template");
	alert_url_ring_check("http://127.0.0.1/nortel/splash_ring", 5);

	CASE("nortel: dst_ring_4 and dst_ring_5 take the rows above the splash");
	alert_url_ring_check("http://127.0.0.1/nortel/dst_ring_4", 6);
	alert_url_ring_check("http://127.0.0.1/nortel/dst_ring_5", 7);

	CASE("nortel: dst_ring_6 to dst_ring_9 have no template and trace once");
	alert_url_ring_check("http://127.0.0.1/nortel/dst_ring_6", 0);
	CHECK_UINT(tr.alert_lines, 1);
	CHECK(strstr(tr.alert, "alert pattern ") != NULL);
	CHECK(strstr(tr.alert, "dst_ring_6") != NULL);
	alert_url_ring_check("http://127.0.0.1/nortel/dst_ring_7", 0);
	alert_url_ring_check("http://127.0.0.1/nortel/dst_ring_8", 0);
	alert_url_ring_check("http://127.0.0.1/nortel/dst_ring_9", 0);

	CASE("nortel: a digit out of range falls through to the token forms");
	alert_url_ring_check("http://127.0.0.1/nortel/dst_ring_0/splash_ring", 5);
	alert_url_ring_check("http://127.0.0.1/nortel/dst_ring_0/standard_ring", 1);
	alert_url_ring_check("http://127.0.0.1/nortel/dst_ring_0", 0);

	/* one find per sentinel, so the second dst_ring_ is unread */
	CASE("nortel: the first sentinel occurrence decides");
	alert_url_ring_check("http://127.0.0.1/nortel/dst_ring_0/dst_ring_3", 0);
	CHECK(strstr(tr.alert, "alert info ") != NULL);

	CASE("nortel: the sentinels are not read from the parameters");
	alert_ring_check("dst_ring_3", 0);
	alert_ring_check("splash_ring", 0);

	CASE("nz: da1 to da4 are the templates themselves");
	alert_url_ring_check("http://127.0.0.1/da1", 1);
	alert_url_ring_check("http://127.0.0.1/da2", 2);
	alert_url_ring_check("http://127.0.0.1/da3", 3);
	alert_url_ring_check("http://127.0.0.1/da4", 4);

	CASE("nz: the sentinel is anchored, so a file name selects no ring");
	alert_url_ring_check("http://host/veranda3.wav", 0);
	alert_url_ring_check("http://host/monday3.wav", 0);

	CASE("nz: all five anchor characters and the start of the URL ring as well");
	alert_url_ring_check("http://host/x?ring=da2", 2);
	alert_url_ring_check("http://host/ring-da1", 1);
	alert_url_ring_check("da4", 4);
	alert_url_ring_check("http://host/play?da3", 3);
	alert_url_ring_check("http://host/x?a=1&da2", 2);

	CASE("nz: an anchored sentinel still needs the digit right behind it");
	alert_url_ring_check("http://host/x?data=1", 0);
	alert_url_ring_check("http://host/x?date=3", 0);
	alert_url_ring_check("http://host/x?a=1&day=x", 0);

	CASE("nz: the sentinel is not read from the parameters");
	alert_ring_check("da2", 0);

	CASE("several Alert-Info headers are one walk");
	alert_hdr_ring_check("Alert-Info: <http://host/ring.wav>\r\n"
			     "Alert-Info: <http://127.0.0.1/Bellcore-dr3>\r\n", 3);

	CASE("precedence: the first value that names a template wins");
	alert_hdr_ring_check("Alert-Info: <http://host/ring.wav>, "
			     "<http://host/x.wav>;info=alert-autoanswer, "
			     "<http://127.0.0.1/Bellcore-dr4>\r\n", 4);
	CHECK_UINT(tr.alert_lines, 0);

	CASE("precedence: the FIRST of two mapped values wins, one header");
	alert_hdr_ring_check("Alert-Info: <http://127.0.0.1/Bellcore-dr2>, "
			     "<http://127.0.0.1/Bellcore-dr4>\r\n", 2);

	CASE("precedence: the FIRST of two mapped values wins across headers and dialects");
	alert_hdr_ring_check("Alert-Info: <http://127.0.0.1/da3>\r\n"
			     "Alert-Info: <http://127.0.0.1/nortel/splash_ring>\r\n", 3);

	CASE("precedence: with nothing mapped the first value with a dialect is traced");
	alert_hdr_ring_check("Alert-Info: <http://host/ring.wav>, "
			     "<http://host/x.wav>;info=alert-autoanswer, "
			     "<http://host/other.wav>\r\n", 0);
	CHECK_UINT(tr.alert_lines, 1);
	CHECK(strstr(tr.alert, "alert pattern ") != NULL);
	CHECK(strstr(tr.alert, "alert-autoanswer") != NULL);

	CASE("precedence: the FIRST value with a dialect is the one traced");
	alert_hdr_ring_check("Alert-Info: <http://host/a.wav>;info=alert-autoanswer, "
			     "<http://host/b.wav>;info=alert-visual\r\n", 0);
	CHECK_UINT(tr.alert_lines, 1);
	CHECK(strstr(tr.alert, "alert-autoanswer") != NULL);
	CHECK(strstr(tr.alert, "alert-visual") == NULL);

	CASE("precedence: with no dialect at all the first value that parsed is traced");
	alert_hdr_ring_check("Alert-Info: <http://host/first.wav>, "
			     "<http://host/second.wav>\r\n", 0);
	CHECK_UINT(tr.alert_lines, 1);
	CHECK(strstr(tr.alert, "alert info ") != NULL);
	CHECK(strstr(tr.alert, "first.wav") != NULL);

	CASE("no Alert-Info at all leaves the ring id at zero");
	fx_reset();
	rx(invite_text("call-alert-none", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK_UINT(ev.incoming, 1);
	CHECK_UINT(ev.ring_id, 0);
	CHECK_UINT(tr.alert_lines, 0);
}

/*
 * Caller identity, both directions.
 *
 * Inbound. The leg keeps what the SIP layer parsed, and reports the privacy
 * verdict beside it. Reason: blank strings would lose the one thing that the
 * backend needs to tell a withheld identity from an absent one. They would
 * also hide the number from the status page and the trace.
 *
 * Outbound. A withheld call sends the RFC 3323 anonymous identity plus a
 * Privacy header. The From is the whole anonymization: no
 * P-Asserted-Identity is asserted, because a CPE has no trust domain to
 * assert one into.
 */
static void test_cid(void)
{
	struct req_info ri;
	struct slot_view sv;

	fx_reset();

	CASE("cid: a Privacy: id INVITE keeps the identity and flags it withheld");
	rx(invite_text("call-cid1", "Privacy: id\r\n", sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK_UINT(ev.incoming, 1);
	CHECK(!strcmp(ev.cid_number, "1002"));
	CHECK(!strcmp(ev.cid_name, "Alice"));
	CHECK(ev.cid_number_private);
	CHECK(ev.cid_name_private);

	CASE("cid: the status dump keeps the real identity and names the flag");
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.remote, "1002"));
	CHECK(!strcmp(sv.name, "Alice"));
	CHECK(sv.cid_private);

	CASE("cid: an ordinary caller carries neither flag");
	fx_reset();
	rx(invite_text("call-cid2", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK_UINT(ev.incoming, 1);
	CHECK(!ev.cid_number_private);
	CHECK(!ev.cid_name_private);
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!sv.cid_private);

	/*
	 * The RFC 3323 anonymous From carries the literal "anonymous" as its
	 * user part, so the leg reports that literal, with both flags set. It
	 * is not an identity and the subscriber never sees it: the flags make
	 * the backend render the withheld reason letter instead of any field
	 * value. Nothing downstream reads the string when the flag is set.
	 */
	CASE("cid: an anonymous caller reports the placeholder with both flags set");
	fx_reset();
	rx(invite_text_from("call-cid3",
			    "\"Anonymous\" <sip:anonymous@anonymous.invalid>", NULL,
			    sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK_UINT(ev.incoming, 1);
	CHECK(!strcmp(ev.cid_number, "anonymous"));
	CHECK(ev.cid_number_private);
	CHECK(ev.cid_name_private);

	CASE("cid: an anonymous From with no user part reports empty strings");
	fx_reset();
	rx(invite_text_from("call-cid3b", "\"Anonymous\" <sip:anonymous.invalid>", NULL,
			    sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK_UINT(ev.incoming, 1);
	CHECK(!ev.cid_number[0]);
	CHECK(ev.cid_number_private);
	CHECK(ev.cid_name_private);

	/* the P-Asserted-Identity carries the identity the network vouches for,
	 * and Privacy: id says the subscriber may not be shown it (RFC 3325) */
	CASE("cid: a private P-Asserted-Identity keeps the asserted number");
	fx_reset();
	rx(invite_text("call-cid4",
		       "P-Asserted-Identity: \"Real Name\" <sip:5551000@" T_DOMAIN ">\r\n"
		       "Privacy: id\r\n",
		       sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK_UINT(ev.incoming, 1);
	CHECK(!strcmp(ev.cid_number, "5551000"));
	CHECK(!strcmp(ev.cid_name, "Real Name"));
	CHECK(ev.cid_number_private);

	/*
	 * The message of the field case. Its display name is the number with
	 * the STIR/SHAKEN verstat parameter behind it, so the name is dropped
	 * and the number alone reaches the line.
	 */
	CASE("cid: the field INVITE reports the number with no name at all");
	fx_reset();
	rx(invite_text_field("call-cid5"));
	CHECK_UINT(ev.incoming, 1);
	CHECK(!strcmp(ev.cid_number, "2015550123"));
	CHECK(!ev.cid_name[0]);
	CHECK(!ev.cid_number_private);
	CHECK(!ev.cid_name_private);

	CASE("cid: and its 180 echoes the field To with our tag behind it");
	CHECK(call_ring(0, 0));
	CHECK(has(send_text(0), "SIP/2.0 180"));
	CHECK(has(send_text(0), "To: sip:oRYOOqsGFzQpUt2dOjeCRZ0"
				"@203.0.113.251:10311;transport=udp;tag="));

	CASE("cid: a presented outbound call sends the account identity");
	fx_reset();
	CHECK(dial_present(0, 0, "1002", true));
	CHECK(last_req(&ri));
	CHECK(has(ri.from, "sip:1001@" T_DOMAIN));
	CHECK(has(ri.from, "Test Line"));
	CHECK(ri.from_tag[0] != 0);

	CASE("cid: and no Privacy header at all");
	CHECK(!has(send_text(0), "Privacy:"));

	CASE("cid: a withheld outbound call sends the RFC 3323 anonymous From");
	fx_reset();
	CHECK(dial_present(0, 0, "1002", false));
	CHECK(last_req(&ri));
	CHECK(has(send_text(0),
		  "From: \"Anonymous\" <sip:anonymous@anonymous.invalid>;tag="));
	CHECK(!has(ri.from, "1001"));

	CASE("cid: with the Privacy header beside it");
	CHECK(has(send_text(0), "Privacy: id\r\n"));

	CASE("cid: the From still carries a tag of its own");
	CHECK(ri.from_tag[0] != 0);

	/* media has to reach this line whatever the display shows, and a
	 * request URI that named nobody would reach nobody */
	CASE("cid: the Contact and the request URI stay real");
	CHECK(!strcmp(ri.contact, "sip:1001@" T_HOST ":5060"));
	CHECK(!strcmp(ri.ruri, "sip:1002@" T_DOMAIN));

	CASE("cid: nothing else about the INVITE changes");
	CHECK(has(send_text(0), "Allow: " UA_ALLOW_DEFAULT));
	CHECK(has(send_text(0), "Supported: timer"));
	CHECK(has(ri.body, "m=audio 10000 RTP/AVP 0 8 18 101"));

	CASE("cid: the withheld call sets up and answers like any other");
	reply(100, NULL, NULL, NULL);
	reply(200, "peer-tag", NULL, sdp_body(T_PEER, 20000, "0", NULL));
	CHECK_UINT(ev.answered, 1);
	CHECK_UINT(med.start_calls, 1);

	/* the dialog copies its local URI from the INVITE we sent, so the
	 * identity cannot drift halfway through the call */
	CASE("cid: a re-INVITE keeps the anonymous identity it started with");
	CHECK(call_hold(0, 0));
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.method, "INVITE"));
	CHECK_UINT(ri.cseq, 2);
	CHECK(has(ri.from, "sip:anonymous@anonymous.invalid"));
	CHECK(!has(ri.from, "1001"));

	CASE("cid: and a presented call keeps its own identity the same way");
	fx_reset();
	uac_up(NULL);
	CHECK(call_hold(0, 0));
	CHECK(last_req(&ri));
	CHECK(has(ri.from, "sip:1001@" T_DOMAIN));

	CASE("cid: an auth retry of a withheld INVITE stays anonymous");
	fx_reset();
	CHECK(dial_present(0, 0, "1002", false));
	reply(401, "peer-tag",
	      "WWW-Authenticate: Digest realm=\"" T_DOMAIN "\", nonce=\"" T_NONCE "\"\r\n",
	      NULL);
	CHECK_UINT(count_send("INVITE"), 2);
	CHECK(last_req(&ri));
	CHECK(has(ri.from, "sip:anonymous@anonymous.invalid"));
	CHECK(has(last_text(), "Privacy: id\r\n"));
}

/*
 * The caller of a refused call. Each of the three pre-ring refusals allocates
 * a leg and releases it at once, and the release reports that leg exactly
 * like any other. The identity is parsed in front of all three. A report that
 * the subscriber never heard about therefore still names who called. An empty
 * remote_number there would say that the caller had none.
 */
static void test_cid_refused(void)
{
	CASE("cid refusal: an unusable offer reports the caller number and name");
	fx_reset();
	rx(invite_text("call-cr1", NULL, sdp_body("sip.example.com", 20000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 488"));
	CHECK_UINT(ev.incoming, 0);
	CHECK_UINT(ev.ended, 1);
	CHECK(!strcmp(ev.end.remote_number, "1002"));
	CHECK(!strcmp(ev.end.remote_name, "Alice"));

	CASE("cid refusal: a body we cannot parse reports them the same way");
	fx_reset();
	rx(invite_text("call-cr2", NULL, "this is not an sdp body"));
	CHECK(has(last_text(), "SIP/2.0 488"));
	CHECK_UINT(ev.incoming, 0);
	CHECK_UINT(ev.ended, 1);
	CHECK(!strcmp(ev.end.remote_number, "1002"));
	CHECK(!strcmp(ev.end.remote_name, "Alice"));

	/* the media take runs in front of the offer, and its refusal is the
	 * first of the three: the identity has to be read before it */
	CASE("cid refusal: a line with no codec left reports them too");
	fx_reset();
	cfg.codecs = 1;
	memset(&cfg.codec[0], 0, sizeof(cfg.codec[0]));
	cfg.codec[0].channel = 1;		/* channel id 0, and disabled */
	rx(invite_text("call-cr3", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 486"));
	CHECK_UINT(ev.incoming, 0);
	CHECK_UINT(ev.ended, 1);
	CHECK(!strcmp(ev.end.remote_number, "1002"));
	CHECK(!strcmp(ev.end.remote_name, "Alice"));
	cfg.codecs = 0;
	cfg.codec[0].channel = 0;

	/*
	 * The inbound rule holds for a call that never rang. The report keeps
	 * what the SIP layer parsed, because the record layer is the daemon and
	 * not the subscriber; the flags that say what the subscriber may see
	 * travel on the incoming callback, which a refusal never makes.
	 */
	CASE("cid refusal: a withheld caller refused before the ring keeps the identity");
	fx_reset();
	rx(invite_text("call-cr4", "Privacy: id\r\n",
		       sdp_body("sip.example.com", 20000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 488"));
	CHECK_UINT(ev.ended, 1);
	CHECK(!strcmp(ev.end.remote_number, "1002"));
	CHECK(!strcmp(ev.end.remote_name, "Alice"));
	CHECK_UINT(ev.incoming, 0);

	CASE("cid refusal: an anonymous caller reports the placeholder it sent");
	fx_reset();
	rx(invite_text_from("call-cr5", "\"Anonymous\" <sip:anonymous@anonymous.invalid>",
			    NULL, sdp_body("sip.example.com", 20000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 488"));
	CHECK_UINT(ev.ended, 1);
	CHECK(!strcmp(ev.end.remote_number, "anonymous"));
	CHECK(!strcmp(ev.end.remote_name, "Anonymous"));

	CASE("cid refusal: a withheld caller that rings still carries both flags");
	fx_reset();
	rx(invite_text("call-cr6", "Privacy: id\r\n", sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK_UINT(ev.incoming, 1);
	CHECK(!strcmp(ev.cid_number, "1002"));
	CHECK(!strcmp(ev.cid_name, "Alice"));
	CHECK(ev.cid_number_private);
	CHECK(ev.cid_name_private);

	CASE("cid refusal: and its own report carries the strings when it ends");
	CHECK(call_reject(0, 0, 486));
	CHECK_UINT(ev.ended, 1);
	CHECK(!strcmp(ev.end.remote_number, "1002"));
	CHECK(!strcmp(ev.end.remote_name, "Alice"));

	/* the identity is read once and nothing after it clears or re-reads it:
	 * the ring presents exactly what it presented before */
	CASE("cid refusal: an offer nothing refuses rings with the same identity");
	fx_reset();
	rx(invite_text("call-cr7", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK_UINT(ev.incoming, 1);
	CHECK(!strcmp(ev.cid_number, "1002"));
	CHECK(!strcmp(ev.cid_name, "Alice"));
	CHECK(!ev.cid_number_private);
	CHECK(!ev.cid_name_private);
	CHECK_UINT(ev.ring_id, 0);

	/* the ring template is read at the ring, so this pins the late read: the
	 * identity moved in front of the refusals and the ring id did not */
	CASE("cid refusal: and the Alert-Info ring template arrives with it");
	fx_reset();
	rx(invite_text("call-cr8", "Alert-Info: <http://host/x.wav>;info=alert-external\r\n",
		       sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK_UINT(ev.incoming, 1);
	CHECK_UINT(ev.ring_id, 2);
	CHECK(!strcmp(ev.cid_number, "1002"));

	/*
	 * The other half of that pin. A leg that never rings has no use for a
	 * ring template, and the trace of an Alert-Info that names none would
	 * report a ring the subscriber never heard. The refusal reads the
	 * header not at all: the alert trace of the whole case is the proof,
	 * because this token names no ring and would make one line if the walk
	 * ran.
	 */
	CASE("cid refusal: a refused leg never reads the Alert-Info at all");
	fx_reset();
	rx(invite_text("call-cr9", "Alert-Info: <http://host/x.wav>;info=alert-autoanswer\r\n",
		       sdp_body("sip.example.com", 20000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 488"));
	CHECK_UINT(ev.ended, 1);
	CHECK_UINT(tr.alert_lines, 0);
}

static void test_misc(void)
{
	struct slot_view sv;
	struct blob_buf b;

	CASE("dtmf: RFC 2833 goes to the stream");
	fx_reset();
	uac_up(NULL);
	CHECK(call_dtmf(0, 0, '5', 100, false));
	CHECK_UINT(med.dtmf_calls, 1);
	CHECK(med.last_digit == '5');
	CHECK_UINT(med.last_duration, 100);

	CASE("dtmf: a digit outside the alphabet is refused");
	CHECK(!call_dtmf(0, 0, 'Z', 100, false));
	CHECK_UINT(med.dtmf_calls, 1);

	CASE("dtmf: a digit before the call is up is refused");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	CHECK(!call_dtmf(0, 0, '5', 100, false));

	CASE("dtmf: the INFO body carries the duration supplied by the line layer");
	fx_reset();
	cfg.channel[0].dtmf_mode = CFG_DTMF_INFO;
	uac_up(NULL);
	CHECK(call_dtmf(0, 0, '7', 120, false));
	CHECK(find_send("INFO") > 0);
	CHECK(has(last_text(), "Signal=7"));
	CHECK(has(last_text(), "Duration=120"));
	CHECK_UINT(med.dtmf_calls, 0);

	CASE("dtmf: inband mode sends nothing at all");
	fx_reset();
	cfg.channel[0].dtmf_mode = CFG_DTMF_INBAND;
	uac_up(NULL);
	CHECK(call_dtmf(0, 0, '3', 100, false));
	CHECK(find_send("INFO") < 0);
	CHECK_UINT(med.dtmf_calls, 0);
	cfg.channel[0].dtmf_mode = CFG_DTMF_RFC2833;

	/*
	 * A backend that advertises HAL_FEAT_DTMF_RELAY puts the event on the
	 * wire itself while the tone sounds, so a second one from here sends
	 * the digit twice. The rule covers a measured digit alone: a pulse
	 * digit and an injected digit never reach the backend's detector.
	 */
	CASE("dtmf: a measured digit is not sent again when the backend "
	     "relays it");
	fx_reset();
	calls_hal_features_set(HAL_FEAT_DTMF_RELAY);
	uac_up(NULL);
	CHECK_UINT(med.last.dtmf_mode, HAL_DTMF_RFC2833);
	CHECK(call_dtmf(0, 0, '5', 100, true));
	CHECK_UINT(med.dtmf_calls, 0);

	CASE("dtmf: a pulse digit still reaches the stream under the native "
	     "relay");
	CHECK(call_dtmf(0, 0, '5', 0, false));
	CHECK_UINT(med.dtmf_calls, 1);

	CASE("dtmf: the INFO body still goes out under the native relay");
	fx_reset();
	calls_hal_features_set(HAL_FEAT_DTMF_RELAY);
	cfg.channel[0].dtmf_mode = CFG_DTMF_INFO;
	uac_up(NULL);
	CHECK(call_dtmf(0, 0, '7', 120, true));
	CHECK(find_send("INFO") > 0);
	CHECK(has(last_text(), "Signal=7"));
	CHECK(has(last_text(), "Duration=120"));
	CHECK_UINT(med.dtmf_calls, 0);
	cfg.channel[0].dtmf_mode = CFG_DTMF_RFC2833;

	CASE("dtmf: without the bit a measured digit goes to the stream");
	fx_reset();
	uac_up(NULL);
	CHECK(call_dtmf(0, 0, '5', 100, true));
	CHECK_UINT(med.dtmf_calls, 1);
	calls_hal_features_set(0);

	CASE("teardown: a line teardown BYEs the active leg and frees the UI slot");
	fx_reset();
	uac_up(NULL);
	calls_line_teardown(0);
	CHECK(find_send("BYE") > 0);
	CHECK_UINT(med.stop_calls, 1);
	CHECK(slot_is(1));

	CASE("teardown: the leg goes when its BYE transaction ends");
	reply(200, NULL, NULL, NULL);
	fake_advance(6000);
	CHECK(slot_is(0));

	CASE("teardown: an early outbound leg is cancelled");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	reply(100, NULL, NULL, NULL);
	calls_line_teardown(0);
	CHECK(find_send("CANCEL") > 0);
	CHECK(slot_is(1));

	CASE("teardown: the canceled INVITE still sends the ACK for the 487");
	reply_at(0, 487, "peer-tag", NULL, NULL);
	CHECK(find_send("ACK") > 0);
	CHECK(slot_is(0));

	CASE("teardown: a second teardown ends a leg that is still draining");
	fx_reset();
	uac_up(NULL);
	calls_line_teardown(0);
	CHECK(slot_is(1));
	calls_line_teardown(0);
	CHECK(slot_is(0));

	CASE("teardown: an unanswered inbound leg is refused with 480");
	fx_reset();
	rx(invite_text("call-t1", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(call_ring(0, 0));
	calls_line_teardown(0);
	CHECK(has(last_text(), "SIP/2.0 480"));
	CHECK(slot_is(0));

	CASE("teardown: a teardown of an idle line is harmless");
	calls_line_teardown(0);
	calls_line_teardown(CFG_CHANNEL_MAX);
	CHECK(slot_is(0));

	CASE("status: an active leg is reported");
	fx_reset();
	uac_up(NULL);
	memset(&b, 0, sizeof(b));
	blob_buf_init(&b, 0);
	calls_status_dump(&b, 0);
	CHECK(blob_len(b.head) > 0);
	blob_buf_free(&b);

	/*
	 * The packetization the DSP runs is the committed one, not the configured
	 * one: a peer that answers a=ptime:30 moves it, and the status page has no
	 * other way to see what the stream carries.
	 */
	CASE("status: the committed codec and ptime reach the leg entry");
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.codec, "PCMU"));
	CHECK(sv.has_ptime);
	CHECK_UINT(sv.ptime, 20);

	CASE("status: a negotiated ptime is reported, not the configured one");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	reply(100, NULL, NULL, NULL);
	reply(200, "peer-tag", NULL, sdp_body(T_PEER, 20000, "0 101", "a=ptime:30\r\n"));
	CHECK(slot_view_get(0, 0, &sv));
	CHECK_UINT(sv.ptime, 30);

	CASE("status: a leg with nothing committed reports no codec and no ptime");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!sv.codec[0]);
	CHECK(!sv.has_ptime);

	CASE("status: a line out of range dumps nothing");
	memset(&b, 0, sizeof(b));
	blob_buf_init(&b, 0);
	calls_status_dump(&b, CFG_CHANNEL_MAX);
	CHECK_UINT(blob_len(b.head), 0);
	blob_buf_free(&b);

	CASE("commands: a slot out of range is always refused");
	CHECK(!call_dial(0, CALL_SLOTS, "1002", true));
	CHECK(!call_dial(CFG_CHANNEL_MAX, 0, "1002", true));
	CHECK(!call_ring(0, CALL_SLOTS));
	CHECK(!call_answer(CFG_CHANNEL_MAX, 0));
	CHECK(!call_reject(0, CALL_SLOTS, 486));
	CHECK(!call_dtmf(CFG_CHANNEL_MAX, 0, '1', 100, false));

	CASE("commands: a free slot refuses everything but a dial");
	fx_reset();
	CHECK(!call_ring(0, 0));
	CHECK(!call_answer(0, 0));
	CHECK(!call_reject(0, 0, 486));
	CHECK(!call_hangup(0, 0));

	CASE("routing: a response for a leg that is gone is dropped");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	call_txn_resp(0x00990001, 1, NULL);
	call_txn_done(0x00990001, 1, SIP_TXN_DONE_OK);
	call_ua_ack_missing(0x00990001);
	call_ua_glare_retry(0x00990001);
	CHECK_UINT(ev.failed, 0);

	CASE("routing: the slot handle a new INVITE gets is the one it lands on");
	fx_reset();
	CHECK_UINT(call_slot_handle(0, 0), 0x00000001);
	CHECK_UINT(call_slot_handle(0, 1), 0x00010001);
	CHECK_UINT(call_slot_handle(1, 0), 0x00020001);
	CHECK_UINT(call_slot_handle(CFG_CHANNEL_MAX, 0), SIP_HANDLE_NONE);

	CASE("routing: a leg that came and went bumps its generation");
	CHECK(call_dial(0, 0, "1002", true));
	CHECK(call_hangup(0, 0));
	fake_advance(40000);
	CHECK_UINT(call_slot_handle(0, 0), 0x00000002);

	CASE("routing: there is no local tag before the leg answers a ring");
	fx_reset();
	CHECK(call_txn_local_tag(0, 1) == NULL);
	call_rx_cancel(0, 1);
	CHECK_UINT(ev.remote_cancel, 0);

	CASE("reload: a new config governs the next leg only");
	fx_reset();
	uac_up(NULL);
	cfg.channel[0].rtp_port_min = 20000;
	cfg.channel[0].rtp_port_max = 20030;
	calls_reload(&cfg);
	CHECK_UINT(med.last.local.port, 10000);
	CHECK(call_dial(0, 1, "1005", true));
	CHECK(has(last_text(), "m=audio 10002"));
	cfg.channel[0].rtp_port_min = 10000;
	cfg.channel[0].rtp_port_max = 10030;

	CASE("stray: a 2xx from a second fork is acknowledged and ended");
	fx_reset();
	uac_up(NULL);
	{
		struct req_info inv;

		CHECK(req_info_at(0, &inv));
		rx(resp_text(&inv, 200, "fork-tag", NULL, sdp_body(T_PEER, 20500, "0", NULL)));
	}
	CHECK_UINT(count_send("BYE"), 1);
	CHECK_UINT(ev.failed, 0);
	CHECK_UINT(ev.answered, 1);

	CASE("stray: a repeated fork 2xx is cleared again, never absorbed");
	{
		struct req_info inv;

		CHECK(req_info_at(0, &inv));
		rx(resp_text(&inv, 200, "fork-tag", NULL, sdp_body(T_PEER, 20500, "0", NULL)));
	}
	CHECK_UINT(count_send("BYE"), 2);
	CHECK_UINT(ev.failed, 0);

	CASE("stray: a THIRD fork is cleared too, not swallowed by the second");
	{
		struct req_info inv;

		CHECK(req_info_at(0, &inv));
		rx(resp_text(&inv, 200, "fork-three", NULL,
			     sdp_body(T_PEER, 20600, "0", NULL)));
	}
	CHECK_UINT(count_send("BYE"), 3);
	CHECK_UINT(ev.answered, 1);
	CHECK_UINT(ev.failed, 0);
}

/*
 * Edge cases of the leg life cycle, one numbered group each. The first
 * groups cover the shared socket routing, the 480 of a line that is going
 * away, the reload drain and the deferred CANCEL. Then come timer C, the early
 * BYE, the refresh 422 retry, a hangup during our re-INVITE and the inbound
 * re-INVITE commit. Then the DTMF dispatch, the remote target rules, a stream
 * that the HAL refuses and the cached credentials. The last groups cover the
 * retained ACK bytes, the RFC 4028 floor, the RTCP interval cap and the
 * teardown on an address change.
 */
static void test_rework(void)
{
	struct req_info ri;
	struct hal_media m;
	struct sdp_result res;
	struct cfg_channel ch;
	unsigned int slot = 99;
	int ridx;

	/* 1. route a shared socket by dialog, CANCEL target, then request URI */
	CASE("route: an INVITE for the second line lands on the second line");
	fx_reset();
	fk.line = 0;
	rx(invite_text_to(T_USER2, "call-rt1", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK_UINT(ev.incoming, 1);
	CHECK_UINT(ev.line, 1);
	CHECK(!call_slot_free(1, &slot) || slot == 1);

	CASE("route: the first line kept both its slots");
	CHECK(call_slot_free(0, &slot));
	CHECK_UINT(slot, 0);

	CASE("route: an in-dialog request follows its dialog, not the socket");
	CHECK(call_ring(1, 0));
	CHECK(call_answer(1, 0));
	rx(uas_in_dialog("ACK", 1, NULL, NULL));
	rx(uas_in_dialog("BYE", 2, NULL, NULL));
	CHECK_UINT(ev.remote_bye, 1);
	CHECK_UINT(ev.line, 1);

	CASE("route: a CANCEL follows the INVITE it targets");
	fx_reset();
	fk.line = 0;
	rx(invite_text_to(T_USER2, "call-rt2", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(call_ring(1, 0));
	rx(cancel_text("call-rt2"));
	CHECK_UINT(ev.remote_cancel, 1);
	CHECK_UINT(ev.line, 1);

	CASE("route: a request URI we do not serve falls back to the socket owner");
	fx_reset();
	fk.line = 1;
	rx(invite_text_to("9999", "call-rt3", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK_UINT(ev.incoming, 1);
	CHECK_UINT(ev.line, 1);

	/* 2. a line that is going away answers 480, not 486 */
	CASE("drain: a new INVITE while the account unregisters is answered 480");
	fx_reset();
	fk.active = false;
	rx(invite_text("call-dr1", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 480"));
	CHECK_UINT(ev.incoming, 0);
	CHECK(slot_is(0));

	CASE("drain: the call layer refuses it even if account.c lets it through");
	fx_reset();
	fk.active = false;
	{
		struct sip_msg *m2 = msg_take(invite_text("call-dr2", NULL,
							  sdp_body(T_PEER, 20000, "0", NULL)));
		struct sip_txn *t;

		CHECK(m2 != NULL);
		t = m2 ? sip_txn_server_start(&mgr, m2, &fk.peer,
					      acct_owner_make(ACCT_OWNER_CALL,
							      call_slot_handle(0, 0))) : NULL;
		CHECK(t != NULL);
		if (t)
			call_rx_request(0, t, m2, &fk.peer);
	}
	CHECK(has(last_text(), "SIP/2.0 480"));
	CHECK_UINT(ev.incoming, 0);

	/* 3. the reload drain waits for the call drain */
	CASE("drain: a line with a leg still on the wire reports busy");
	fx_reset();
	uac_up(NULL);
	CHECK(calls_line_busy(0));
	CHECK(call_hangup(0, 0));
	CHECK(calls_line_busy(0));

	CASE("drain: it reports idle again when the BYE transaction ends");
	reply(200, NULL, NULL, NULL);
	fake_advance(6000);
	CHECK(!calls_line_busy(0));
	CHECK(!calls_line_busy(CFG_CHANNEL_MAX));

	/* 5. a deferred CANCEL is a clearing state everywhere */
	CASE("clearing: a 2xx that beats the deferred CANCEL is ACKed and ended");
	fx_reset();
	CHECK(dial(0, 0, "1002"));
	CHECK(call_hangup(0, 0));
	reply(200, "peer-tag", NULL, sdp_body(T_PEER, 20000, "0", NULL));
	CHECK(find_send("ACK") > 0);
	CHECK(find_send("BYE") > 0);
	CHECK_UINT(ev.answered, 0);
	CHECK_UINT(med.start_calls, 0);

	CASE("clearing: a 401 after a deferred CANCEL starts no second INVITE");
	fx_reset();
	CHECK(dial(0, 0, "1002"));
	CHECK(call_hangup(0, 0));
	reply(401, "peer-tag", "WWW-Authenticate: Digest realm=\"" T_DOMAIN "\", nonce=\""
	      T_NONCE "\"\r\n", NULL);
	CHECK_UINT(count_send("INVITE"), 1);
	CHECK(slot_is(0));

	CASE("clearing: a 491 after a deferred CANCEL arms no glare retry");
	fx_reset();
	CHECK(dial(0, 0, "1002"));
	CHECK(call_hangup(0, 0));
	reply(491, "peer-tag", NULL, NULL);
	CHECK(!timer_armed(SIP_TIMER_GLARE));
	CHECK_UINT(count_send("INVITE"), 1);

	CASE("clearing: an 18x after a deferred CANCEL rings nothing");
	fx_reset();
	cfg.channel[0].early_media_enabled = true;
	CHECK(dial(0, 0, "1002"));
	CHECK(call_hangup(0, 0));
	reply(183, "peer-tag", NULL, sdp_body(T_PEER, 20000, "0", NULL));
	CHECK_UINT(ev.proceeding, 0);
	CHECK_UINT(med.config_calls, 0);
	CHECK(find_send("CANCEL") > 0);
	cfg.channel[0].early_media_enabled = false;

	/* 6. timer C stops when the CANCEL starts */
	CASE("timer C: the first CANCEL disarms it");
	fx_reset();
	cfg.channel[0].timer_c_ms = 5000;
	CHECK(dial(0, 0, "1002"));
	reply(100, NULL, NULL, NULL);
	CHECK(call_hangup(0, 0));
	CHECK(find_send("CANCEL") > 0);
	CHECK(!timer_armed(SIP_TIMER_CALL_C));

	CASE("timer C: no second CANCEL, so the 487 still gets its ACK");
	reply(200, NULL, NULL, NULL);		/* the CANCEL is answered */
	fake_advance(6000);
	CHECK_UINT(count_send("CANCEL"), 1);
	reply_at(0, 487, "peer-tag", NULL, NULL);
	CHECK(find_send("ACK") > 0);
	cfg.channel[0].timer_c_ms = 0;

	/* 7. a BYE before the answer is a clearing, not a remote_bye */
	CASE("early BYE: a peer BYE in an early dialog fails the call quietly");
	fx_reset();
	CHECK(dial(0, 0, "1002"));
	reply(100, NULL, NULL, NULL);
	reply(180, "peer-tag", NULL, NULL);
	rx(uac_in_dialog("BYE", "peer-tag", 1, NULL, NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(ev.remote_bye, 0);
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.cls, CALL_FAIL_QUIET);

	CASE("early BYE: an answered call still reports a plain remote_bye");
	fx_reset();
	uac_up(NULL);
	rx(uac_in_dialog("BYE", "peer-tag", 2, NULL, NULL));
	CHECK_UINT(ev.remote_bye, 1);
	CHECK_UINT(ev.failed, 0);

	/* 8. a 422 to a session refresh is retried once */
	CASE("refresh 422: the refresh is retried with the interval it demands");
	fx_reset();
	cfg.channel[0].session_max_expires = 1800;
	uac_up("Session-Expires: 1800;refresher=uac\r\n");
	fake_advance(900000);
	CHECK_UINT(count_send("INVITE"), 2);
	reply(422, NULL, "Min-SE: 3600\r\n", NULL);
	CHECK_UINT(count_send("INVITE"), 3);
	CHECK(has(last_text(), "Session-Expires: 3600"));
	CHECK(find_send("BYE") < 0);

	CASE("refresh 422: the retry is still not a feature operation");
	CHECK_UINT(ev.reinvite_done, 0);
	CHECK_UINT(ev.failed, 0);

	CASE("refresh 422: a second 422 gives up and clears the session");
	reply(422, NULL, "Min-SE: 7200\r\n", NULL);
	CHECK_UINT(count_send("INVITE"), 3);
	CHECK(find_send("BYE") > 0);
	CHECK_UINT(ev.cls, CALL_FAIL_QUIET);

	CASE("refresh 422: a Min-SE below the interval is retried unchanged");
	fx_reset();
	cfg.channel[0].session_max_expires = 1800;
	uac_up("Session-Expires: 1800;refresher=uac\r\n");
	fake_advance(900000);
	CHECK_UINT(count_send("INVITE"), 2);
	reply(422, NULL, "Min-SE: 90\r\n", NULL);
	CHECK_UINT(count_send("INVITE"), 3);
	CHECK(has(last_text(), "Session-Expires: 1800"));
	CHECK(has(last_text(), "Min-SE: 90"));
	CHECK(find_send("BYE") < 0);
	CHECK_UINT(ev.failed, 0);

	CASE("refresh 422: that retry still completes the refresh");
	reply(200, NULL, "Session-Expires: 1800;refresher=uac\r\n",
	      sdp_body(T_PEER, 20000, "0 101", NULL));
	CHECK_UINT(ev.failed, 0);
	CHECK_UINT(ev.reinvite_done, 0);
	CHECK_UINT(timer_delay(SIP_TIMER_CALL_SESSION), 900000);
	cfg.channel[0].session_max_expires = 0;

	/* 9. a hangup during our re-INVITE suppresses its completion */
	CASE("hangup: a 2xx for a re-INVITE of a cleared leg is ACKed only");
	fx_reset();
	uac_up(NULL);
	CHECK(call_hold(0, 0));
	ridx = find_send("INVITE");
	CHECK(call_hangup(0, 0));
	CHECK(find_send("BYE") > 0);
	CHECK_UINT(med.stop_calls, 1);
	reply_at((unsigned int)ridx, 200, NULL, NULL,
		 sdp_body(T_PEER, 20000, "0 101", "a=recvonly\r\n"));
	CHECK_UINT(count_send("ACK"), 2);

	CASE("hangup: the re-INVITE restarts no media and no session timer");
	CHECK_UINT(med.start_calls, 1);
	CHECK_UINT(med.stop_calls, 1);
	CHECK(!timer_armed(SIP_TIMER_CALL_SESSION));
	CHECK(!timer_armed(SIP_TIMER_CALL_WATCHDOG));

	CASE("hangup: it reports no operation result and sends no second BYE");
	CHECK_UINT(ev.reinvite_done, 0);
	CHECK_UINT(count_send("BYE"), 1);

	CASE("hangup: a failed re-INVITE of a cleared leg is silent too");
	fx_reset();
	uac_up(NULL);
	CHECK(call_hold(0, 0));
	ridx = find_send("INVITE");
	CHECK(call_hangup(0, 0));
	reply_at((unsigned int)ridx, 488, NULL, NULL, NULL);
	CHECK_UINT(ev.reinvite_done, 0);
	CHECK_UINT(count_send("BYE"), 1);

	/* 10. an accepted inbound re-INVITE is applied at the 2xx */
	CASE("re-INVITE in: the stream follows the answer at the 2xx");
	fx_reset();
	uas_up("call-ri1");
	CHECK_UINT(med.config_calls, 1);
	rx(uas_in_dialog("INVITE", 2, NULL, sdp_body("203.0.113.9", 30000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(med.config_calls, 2);
	CHECK_UINT(med.last.remote.port, 30000);

	CASE("re-INVITE in: the ACK commits without programming again");
	rx(uas_in_dialog("ACK", 2, NULL, NULL));
	CHECK_UINT(med.config_calls, 2);
	CHECK_UINT(ev.failed, 0);

	CASE("re-INVITE in: a session interval on it re-arms at the 2xx");
	fx_reset();
	uas_up("call-ri2");
	rx(uas_in_dialog("INVITE", 2, "Supported: timer\r\nSession-Expires: 1800;refresher=uac\r\n",
			 sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(timer_armed(SIP_TIMER_CALL_WATCHDOG));

	/* 11. DTMF follows the negotiated mode */
	CASE("dtmf: an answer without telephone-event downgrades the dispatch");
	fx_reset();
	CHECK(dial(0, 0, "1002"));
	reply(100, NULL, NULL, NULL);
	reply(200, "peer-tag", NULL, sdp_body(T_PEER, 20000, "0", NULL));
	CHECK_UINT(ev.answered, 1);
	CHECK_UINT(med.last.dtmf_mode, HAL_DTMF_INBAND);
	CHECK_UINT(med.last.dtmf_pt, 0);

	CASE("dtmf: the downgraded leg sends no RFC 2833 event");
	CHECK(call_dtmf(0, 0, '5', 100, false));
	CHECK_UINT(med.dtmf_calls, 0);

	CASE("dtmf: a negotiated telephone-event still reaches the stream");
	fx_reset();
	uac_up(NULL);
	CHECK_UINT(med.last.dtmf_mode, HAL_DTMF_RFC2833);
	CHECK(call_dtmf(0, 0, '5', 100, false));
	CHECK_UINT(med.dtmf_calls, 1);

	/* 12. a refused re-INVITE never moves the remote target */
	CASE("target: a 488 re-INVITE leaves the peer address alone");
	fx_reset();
	uas_up("call-tg1");
	rx(uas_in_dialog("INVITE", 2, "Contact: <sip:1002@203.0.113.44:5099>\r\n",
			 sdp_body(T_PEER, 20000, "99", "a=rtpmap:99 SPEEX/8000\r\n")));
	CHECK(has(last_text(), "SIP/2.0 488"));
	CHECK(call_hangup(0, 0));
	CHECK(req_of("BYE", &ri));
	CHECK(!has(ri.ruri, "203.0.113.44"));

	CASE("target: a 491 re-INVITE leaves it alone too");
	fx_reset();
	uas_up("call-tg2");
	rx(uas_in_dialog("INVITE", 2, NULL, NULL));
	rx(uas_in_dialog("INVITE", 3, "Contact: <sip:1002@203.0.113.55:5099>\r\n",
			 sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 491"));
	rx(uas_in_dialog("ACK", 2, NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(call_hangup(0, 0));
	CHECK(req_of("BYE", &ri));
	CHECK(!has(ri.ruri, "203.0.113.55"));

	CASE("target: an accepted re-INVITE does move it");
	fx_reset();
	uas_up("call-tg3");
	rx(uas_in_dialog("INVITE", 2, "Contact: <sip:1002@203.0.113.66:5099>\r\n",
			 sdp_body(T_PEER, 20000, "0", NULL)));
	rx(uas_in_dialog("ACK", 2, NULL, NULL));
	CHECK(call_hangup(0, 0));
	CHECK(req_of("BYE", &ri));
	CHECK(has(ri.ruri, "203.0.113.66"));

	/* 13. a 2xx to our re-INVITE moves the target before the ACK */
	CASE("target: a moved Contact in a re-INVITE 2xx is used by the ACK");
	fx_reset();
	uac_up(NULL);
	CHECK(call_hold(0, 0));
	reply(200, NULL, "Contact: <sip:1002@203.0.113.77:5099>\r\n",
	      sdp_body(T_PEER, 20000, "0 101", "a=recvonly\r\n"));
	CHECK(req_of("ACK", &ri));
	CHECK(has(ri.ruri, "203.0.113.77"));

	CASE("target: and by the BYE that follows it");
	CHECK(call_hangup(0, 0));
	CHECK(req_of("BYE", &ri));
	CHECK(has(ri.ruri, "203.0.113.77"));

	/* 15. a stream the HAL refuses fails the operation */
	CASE("media: a refused re-INVITE stream clears the call");
	fx_reset();
	uac_up(NULL);
	med.config_ret = HAL_FATAL;
	CHECK(call_hold(0, 0));
	reply(200, NULL, NULL, sdp_body(T_PEER, 21000, "0 101", "a=recvonly\r\n"));
	CHECK(find_send("BYE") > 0);
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.cls, CALL_FAIL_CONGESTION);

	CASE("media: the cache did not take the description the HAL refused");
	CHECK_UINT(med.last.remote.port, 21000);
	med.config_ret = HAL_OK;

	CASE("media: a refused answer stream clears the call at once");
	fx_reset();
	med.config_ret = HAL_FATAL;
	CHECK(dial(0, 0, "1002"));
	reply(100, NULL, NULL, NULL);
	reply(200, "peer-tag", NULL, sdp_body(T_PEER, 20000, "0 101", NULL));
	CHECK_UINT(ev.failed, 1);
	CHECK(find_send("BYE") > 0);
	med.config_ret = HAL_OK;

	/* 16. cached credentials ride the requests inside the dialog */
	CASE("auth: a re-INVITE carries the credentials the INVITE proved");
	fx_reset();
	CHECK(dial(0, 0, "1002"));
	reply(100, NULL, NULL, NULL);
	reply(401, "peer-tag", "WWW-Authenticate: Digest realm=\"" T_DOMAIN "\", nonce=\""
	      T_NONCE "\"\r\n", NULL);
	reply(100, NULL, NULL, NULL);
	reply(200, "peer-tag", NULL, sdp_body(T_PEER, 20000, "0 101", NULL));
	CHECK_UINT(ev.answered, 1);
	CHECK(call_hold(0, 0));
	CHECK(has(last_text(), "Authorization: Digest"));

	CASE("auth: so does the BYE");
	CHECK(call_hangup(0, 0));
	CHECK(has(last_text(), "Authorization: Digest"));

	CASE("auth: a challenged BYE is answered once with a new CSeq");
	fx_reset();
	uac_up(NULL);
	CHECK(call_hangup(0, 0));
	CHECK(req_of("BYE", &ri));
	reply(407, NULL, "Proxy-Authenticate: Digest realm=\"" T_DOMAIN "\", nonce=\""
	      T_NONCE "\"\r\n", NULL);
	CHECK_UINT(count_send("BYE"), 2);
	CHECK(req_of("BYE", &ri));
	CHECK_UINT(ri.cseq, 3);
	CHECK(has(last_text(), "Proxy-Authorization: Digest"));

	CASE("auth: a second challenge to the BYE gives up");
	reply(407, NULL, "Proxy-Authenticate: Digest realm=\"" T_DOMAIN
	      "\", nonce=\"another-one\"\r\n", NULL);
	CHECK_UINT(count_send("BYE"), 2);
	CHECK(slot_is(0));

	/* 17. a call is not committed without retained ACK bytes */
	CASE("ack: a 2xx we cannot ACK clears the session");
	fx_reset();
	fk.no_contact = true;
	CHECK(dial(0, 0, "1002"));
	reply(100, NULL, NULL, NULL);
	reply(200, "peer-tag", NULL, sdp_body(T_PEER, 20000, "0 101", NULL));
	CHECK_UINT(ev.answered, 0);
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.cls, CALL_FAIL_QUIET);

	CASE("ack: and starts no media on the way out");
	CHECK_UINT(med.start_calls, 0);
	fk.no_contact = false;

	/* 18. the RFC 4028 floor applies to every inbound INVITE */
	CASE("min-se: the 90 second floor holds with nothing configured");
	fx_reset();
	rx(invite_text("call-ms1", "Session-Expires: 60;refresher=uac\r\n",
		       sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 422"));
	CHECK(has(last_text(), "Min-SE: 90"));
	CHECK_UINT(ev.incoming, 0);

	CASE("min-se: a configured floor above 90 wins");
	fx_reset();
	cfg.channel[0].session_min_interval = 900;
	rx(invite_text("call-ms2", "Session-Expires: 600;refresher=uac\r\n",
		       sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 422"));
	CHECK(has(last_text(), "Min-SE: 900"));
	cfg.channel[0].session_min_interval = 0;

	CASE("min-se: an in-dialog re-INVITE is held to the floor as well");
	fx_reset();
	uas_up("call-ms3");
	rx(uas_in_dialog("INVITE", 2, "Session-Expires: 60;refresher=uac\r\n",
			 sdp_body("203.0.113.9", 30000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 422"));
	CHECK(has(last_text(), "Min-SE: 90"));

	CASE("min-se: the refused re-INVITE moved no media");
	CHECK_UINT(med.last.remote.port, 20000);
	CHECK_UINT(med.config_calls, 1);

	/* 19. the RTCP interval cannot wrap the millisecond field */
	CASE("rtcp: a 66 second interval is capped, not wrapped");
	memset(&res, 0, sizeof(res));
	res.codec.id = SDP_CODEC_PCMU;
	res.dtmf_pt = -1;
	res.cn_pt = -1;
	res.ptime_ms = 20;
	/* a valid result always names a remote address and a nonzero port, and
	 * the fill refuses anything else */
	res.remote_port = 20000;
	res.remote_ipver = 4;
	snprintf(res.remote_addr, sizeof(res.remote_addr), "%s", T_PEER);
	res.valid = true;
	ch = cfg.channel[0];
	media_fill(&ch, true, 66, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.rtcp_interval_ms, 65000);

	CASE("rtcp: an enormous interval is capped the same way");
	media_fill(&ch, true, 4000000, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.rtcp_interval_ms, 65000);
	media_fill(&ch, true, 5, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.rtcp_interval_ms, 5000);

	/* 20. an address change clears the calls that copied the old address */
	CASE("address: a teardown tells the FSM the leg is gone");
	fx_reset();
	uac_up(NULL);
	calls_line_teardown(0);
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.cls, CALL_FAIL_QUIET);
	CHECK(find_send("BYE") > 0);

	CASE("address: a leg the FSM already released is not reported again");
	fx_reset();
	uac_up(NULL);
	CHECK(call_hangup(0, 0));
	calls_line_teardown(0);
	CHECK_UINT(ev.failed, 0);
}

/* ------------------------------------- the BroadWorks call waiting INFO */

/*
 * The grammar of the call waiting INFO body. Every body below is copied into
 * a buffer of exactly its own length, so the parser has no terminator to lean
 * on and any read past the body is a heap error under the sanitizer.
 */
static bool cw_parse(const char *text, struct call_cw *out)
{
	size_t len = strlen(text);
	char *buf = malloc(len ? len : 1);
	bool ok;

	memcpy(buf, text, len);
	ok = call_cw_parse(sip_str_init(buf, len), out);
	free(buf);

	return ok;
}

static void test_cw_parse(void)
{
	struct call_cw cw;

	CASE("cw parse: the stop command");
	CHECK(cw_parse("stop CallWaitingTone", &cw));
	CHECK(!cw.play);
	CHECK_UINT(cw.tone, 0);
	CHECK(cw.name[0] == 0);
	CHECK(cw.number[0] == 0);

	CASE("cw parse: every GR-506 pattern of the play command");
	CHECK(cw_parse("play tone CallWaitingTone1", &cw));
	CHECK(cw.play);
	CHECK_UINT(cw.tone, 1);
	CHECK(cw_parse("play tone CallWaitingTone2", &cw));
	CHECK_UINT(cw.tone, 2);
	CHECK(cw_parse("play tone CallWaitingTone3", &cw));
	CHECK_UINT(cw.tone, 3);
	CHECK(cw_parse("play tone CallWaitingTone4", &cw));
	CHECK_UINT(cw.tone, 4);

	CASE("cw parse: the pattern digit is required, bounded and glued");
	CHECK(!cw_parse("play tone CallWaitingTone", &cw));
	CHECK(!cw_parse("play tone CallWaitingTone0", &cw));
	CHECK(!cw_parse("play tone CallWaitingTone5", &cw));
	CHECK(!cw_parse("play tone CallWaitingTone9", &cw));
	CHECK(!cw_parse("play tone CallWaitingTone 1", &cw));

	CASE("cw parse: an empty body carries no command");
	CHECK(!cw_parse("", &cw));

	CASE("cw parse: a body that stops inside the keyword is refused");
	CHECK(!cw_parse("play tone CallWaiting", &cw));
	CHECK(!cw_parse("stop CallWaiting", &cw));
	CHECK(!cw_parse("s", &cw));

	CASE("cw parse: the keywords are case sensitive, the switch generates them");
	CHECK(!cw_parse("Stop CallWaitingTone", &cw));
	CHECK(!cw_parse("STOP CALLWAITINGTONE", &cw));
	CHECK(!cw_parse("play tone callwaitingtone1", &cw));

	CASE("cw parse: an unknown command is refused");
	CHECK(!cw_parse("ring the bell", &cw));
	CHECK(!cw_parse("playstop CallWaitingTone", &cw));

	/* leading whitespace is tolerated, so a switch that wraps its body
	 * still works */
	CASE("cw parse: whitespace and CRLF before the command are tolerated");
	CHECK(cw_parse("\r\n  \tstop CallWaitingTone", &cw));
	CHECK(!cw.play);
	CHECK(cw_parse("  play tone CallWaitingTone2", &cw));
	CHECK_UINT(cw.tone, 2);

	/* the parser matches the prefix and looks no further */
	CASE("cw parse: bytes after the command do not refuse it");
	CHECK(cw_parse("stop CallWaitingTone\r\n", &cw));
	CHECK(cw_parse("play tone CallWaitingTone1\r\nX-Junk: 1\r\n", &cw));
	CHECK_UINT(cw.tone, 1);

	CASE("cw parse: the play carries the calling name and number");
	CHECK(cw_parse("play tone CallWaitingTone2\r\n"
		       "Calling-Name:\"Alice Smith\"\r\n"
		       "Calling-Number:5551000\r\n", &cw));
	CHECK(cw.play);
	CHECK_UINT(cw.tone, 2);
	CHECK(strcmp(cw.name, "Alice Smith") == 0);
	CHECK(strcmp(cw.number, "5551000") == 0);

	CASE("cw parse: the two fields are found in any order and anywhere");
	CHECK(cw_parse("play tone CallWaitingTone1 Calling-Number:5551000 "
		       "Calling-Name:\"Bob\"", &cw));
	CHECK(strcmp(cw.name, "Bob") == 0);
	CHECK(strcmp(cw.number, "5551000") == 0);

	CASE("cw parse: whitespace after the number colon is skipped");
	CHECK(cw_parse("play tone CallWaitingTone1\r\nCalling-Number:  5551000\r\n", &cw));
	CHECK(strcmp(cw.number, "5551000") == 0);

	CASE("cw parse: the number takes digits and stops at the first other byte");
	CHECK(cw_parse("play tone CallWaitingTone1\r\nCalling-Number:555-1000\r\n", &cw));
	CHECK(strcmp(cw.number, "555") == 0);
	CHECK(cw_parse("play tone CallWaitingTone1\r\nCalling-Number:+15551000\r\n", &cw));
	CHECK(cw.number[0] == 0);

	CASE("cw parse: the field keywords are case sensitive too");
	CHECK(cw_parse("play tone CallWaitingTone1\r\ncalling-name:\"Alice\"\r\n"
		       "calling-number:5551000\r\n", &cw));
	CHECK(cw.name[0] == 0);
	CHECK(cw.number[0] == 0);

	/* the two fields are read in the play branch only */
	CASE("cw parse: a stop ignores the two fields");
	CHECK(cw_parse("stop CallWaitingTone\r\nCalling-Name:\"Alice\"\r\n"
		       "Calling-Number:5551000\r\n", &cw));
	CHECK(!cw.play);
	CHECK(cw.name[0] == 0);
	CHECK(cw.number[0] == 0);

	/*
	 * An unclosed quote yields nothing, and every copy is bounded by the
	 * body length.
	 */
	CASE("cw parse: an unclosed name quote yields no name and reads no further");
	CHECK(cw_parse("play tone CallWaitingTone1\r\nCalling-Name:\"Alice", &cw));
	CHECK_UINT(cw.tone, 1);
	CHECK(cw.name[0] == 0);

	CASE("cw parse: a body that ends at the opening quote yields no name");
	CHECK(cw_parse("play tone CallWaitingTone1\r\nCalling-Name:\"", &cw));
	CHECK(cw.name[0] == 0);

	CASE("cw parse: a name keyword with no quote at all yields no name");
	CHECK(cw_parse("play tone CallWaitingTone1\r\nCalling-Name:Alice\r\n", &cw));
	CHECK(cw.name[0] == 0);

	CASE("cw parse: a body that ends at the number colon yields no number");
	CHECK(cw_parse("play tone CallWaitingTone1\r\nCalling-Number:", &cw));
	CHECK(cw.number[0] == 0);

	CASE("cw parse: a body that ends inside the field keyword is harmless");
	CHECK(cw_parse("play tone CallWaitingTone1\r\nCalling-Nam", &cw));
	CHECK(cw.name[0] == 0);
	CHECK(cw_parse("play tone CallWaitingTone1\r\nCalling-Numbe", &cw));
	CHECK(cw.number[0] == 0);

	CASE("cw parse: an empty name is an empty name, not an absent one");
	CHECK(cw_parse("play tone CallWaitingTone1\r\nCalling-Name:\"\"\r\n", &cw));
	CHECK(cw.name[0] == 0);

	CASE("cw parse: a name that exactly fills the field keeps its terminator");
	CHECK(cw_parse("play tone CallWaitingTone1\r\n"
		       "Calling-Name:\"12345678901234567890123456789012\"\r\n", &cw));
	CHECK_UINT(strlen(cw.name), HAL_CID_STR_MAX - 1);
	CHECK(strcmp(cw.name, "12345678901234567890123456789012") == 0);

	CASE("cw parse: an oversized name is truncated silently");
	CHECK(cw_parse("play tone CallWaitingTone1\r\n"
		       "Calling-Name:\"1234567890123456789012345678901234567890\"\r\n",
		       &cw));
	CHECK_UINT(strlen(cw.name), HAL_CID_STR_MAX - 1);
	CHECK(strcmp(cw.name, "12345678901234567890123456789012") == 0);

	CASE("cw parse: an oversized number is truncated silently");
	CHECK(cw_parse("play tone CallWaitingTone1\r\n"
		       "Calling-Number:1234567890123456789012345678901234567890\r\n",
		       &cw));
	CHECK_UINT(strlen(cw.number), HAL_CID_STR_MAX - 1);
	CHECK(strcmp(cw.number, "12345678901234567890123456789012") == 0);

	CASE("cw ctype: the broadsoft media type matches");
	CHECK(call_cw_ctype_match(sip_str_cstr("application/broadsoft")));

	CASE("cw ctype: the media type is matched case insensitively");
	CHECK(call_cw_ctype_match(sip_str_cstr("Application/BroadSoft")));
	CHECK(call_cw_ctype_match(sip_str_cstr("APPLICATION/BROADSOFT")));

	CASE("cw ctype: parameters and surrounding whitespace are tolerated");
	CHECK(call_cw_ctype_match(sip_str_cstr("application/broadsoft;charset=utf-8")));
	CHECK(call_cw_ctype_match(sip_str_cstr(" application/broadsoft ")));
	CHECK(call_cw_ctype_match(sip_str_cstr("application/broadsoft ;x=1")));

	CASE("cw ctype: every other media type is refused");
	CHECK(!call_cw_ctype_match(sip_str_cstr("application/dtmf-relay")));
	CHECK(!call_cw_ctype_match(sip_str_cstr("application/sdp")));
	CHECK(!call_cw_ctype_match(sip_str_cstr("application/broadsoftx")));
	CHECK(!call_cw_ctype_match(sip_str_cstr("broadsoft")));
	CHECK(!call_cw_ctype_match(sip_str_cstr("")));
}

#define CW_CTYPE	"Content-Type: application/broadsoft\r\n"

static void test_cw_wire(void)
{
	unsigned int before;

	fx_reset();
	uas_up("call-cw1");

	CASE("cw info: a body-less INFO keeps the 200 of RFC 2976 2.2");
	rx(uas_in_dialog("INFO", 2, NULL, NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(ev.cw_net, 0);

	CASE("cw info: a broadsoft INFO with no body is answered the same way");
	rx(uas_in_dialog("INFO", 3, CW_CTYPE, NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(ev.cw_net, 0);

	/*
	 * A 400 to every in-dialog INFO whose content type is not broadsoft
	 * would break a call over a body this side simply does not consume.
	 * The type here is one no side of this daemon reads, because the
	 * dtmf-relay one is consumed now.
	 */
	CASE("cw info: a foreign content type keeps the acknowledge and drop");
	rx(uas_in_dialog("INFO", 4, "Content-Type: application/x-foo\r\n",
			 "Signal=5\r\nDuration=160\r\n"));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(ev.cw_net, 0);
	CHECK_UINT(ev.digit_net, 0);

	CASE("cw info: the play reaches the FSM with the parsed identity");
	before = fk.sends;
	rx(uas_in_dialog("INFO", 5, CW_CTYPE,
			 "play tone CallWaitingTone2\r\n"
			 "Calling-Name:\"Alice Smith\"\r\n"
			 "Calling-Number:5551000\r\n"));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(ev.cw_net, 1);
	CHECK(ev.cw_on);
	CHECK_UINT(ev.cw_tone, 2);
	CHECK(strcmp(ev.cw_name, "Alice Smith") == 0);
	CHECK(strcmp(ev.cw_number, "5551000") == 0);
	CHECK_UINT(ev.line, 0);
	CHECK_UINT(ev.slot, 0);

	/* the answer is decided by the body alone, so a state
	 * that drops the alert downstream never changes what the switch sees */
	CASE("cw info: the 200 is on the wire before the alert is dispatched");
	CHECK_UINT(ev.cw_sends, before + 1);

	CASE("cw info: the stop reaches the FSM with no tone and no identity");
	rx(uas_in_dialog("INFO", 6, CW_CTYPE, "stop CallWaitingTone\r\n"));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(ev.cw_net, 2);
	CHECK(!ev.cw_on);
	CHECK_UINT(ev.cw_tone, 0);
	CHECK(ev.cw_name[0] == 0);
	CHECK(ev.cw_number[0] == 0);

	CASE("cw info: a play with no identity is still a play");
	rx(uas_in_dialog("INFO", 7, CW_CTYPE, "play tone CallWaitingTone1\r\n"));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(ev.cw_net, 3);
	CHECK(ev.cw_on);
	CHECK_UINT(ev.cw_tone, 1);
	CHECK(ev.cw_name[0] == 0);

	CASE("cw info: an unreadable broadsoft body is answered 400 and dispatched nowhere");
	rx(uas_in_dialog("INFO", 8, CW_CTYPE, "ring the bell\r\n"));
	CHECK(has(last_text(), "SIP/2.0 400"));
	CHECK_UINT(ev.cw_net, 3);

	CASE("cw info: a bad pattern digit is answered 400 too");
	rx(uas_in_dialog("INFO", 9, CW_CTYPE, "play tone CallWaitingTone9\r\n"));
	CHECK(has(last_text(), "SIP/2.0 400"));
	CHECK_UINT(ev.cw_net, 3);

	CASE("cw info: the media type parameters do not change the answer");
	rx(uas_in_dialog("INFO", 10, "Content-Type: Application/BroadSoft;charset=utf-8\r\n",
			 "play tone CallWaitingTone3\r\n"));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(ev.cw_net, 4);
	CHECK_UINT(ev.cw_tone, 3);

	CASE("cw info: the alert does not touch the media or the slot");
	CHECK_UINT(med.stop_calls, 0);
	CHECK(slot_is(1));

	/* the leg is driven directly, because the account intake in front of it
	 * answers a NOTIFY that names no event package at all (RFC 6665 8.2.1,
	 * 489) and this case is about what the leg does with the body */
	CASE("cw info: another method with the same body carries no alert");
	rx_in_dialog_direct(0, uas_in_dialog("NOTIFY", 11, CW_CTYPE,
					     "play tone CallWaitingTone1\r\n"));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(ev.cw_net, 4);

	/* a body with no readable content type is not a broadsoft one either */
	CASE("cw info: an empty content type keeps the acknowledge and drop");
	rx(uas_in_dialog("INFO", 12, "Content-Type: \r\n", "stop CallWaitingTone\r\n"));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(ev.cw_net, 4);

	CASE("cw info: an INFO for a dialog that does not exist is refused 481");
	fx_reset();
	rx(dialog_req_text("INFO", 9, "no-such-dialog", "peer", "us", CW_CTYPE,
			   "stop CallWaitingTone\r\n"));
	CHECK(has(last_text(), "SIP/2.0 481"));
	CHECK_UINT(ev.cw_net, 0);

	CASE("cw info: an old CSeq is refused before the body is read");
	fx_reset();
	uas_up("call-cw2");
	rx(uas_in_dialog("INFO", 1, CW_CTYPE, "play tone CallWaitingTone1\r\n"));
	CHECK(has(last_text(), "SIP/2.0 500"));
	CHECK_UINT(ev.cw_net, 0);
}

/* ------------------------------------------- the out of band digit INFO */

#define DTMF_RELAY_CTYPE	"Content-Type: application/dtmf-relay\r\n"
#define DTMF_CTYPE		"Content-Type: application/dtmf\r\n"

/*
 * The grammar of the out of band digit body, driven the way the call waiting
 * one is: the body is copied into a buffer of exactly its own length, so a
 * read past it is a heap error under the sanitizer rather than a pass.
 */
static bool dtmf_parse(const char *ctype, const char *text, char *digit,
		       uint16_t *duration_ms)
{
	size_t len = strlen(text);
	char *buf = malloc(len ? len : 1);
	bool ok;

	memcpy(buf, text, len);
	ok = call_dtmf_relay_parse(sip_str_cstr(ctype), sip_str_init(buf, len),
				   digit, duration_ms);
	free(buf);

	return ok;
}

static bool dtmf_relay_parse(const char *text, char *digit,
			     uint16_t *duration_ms)
{
	return dtmf_parse("application/dtmf-relay", text, digit, duration_ms);
}

static void test_dtmf_parse(void)
{
	uint16_t ms = 0;
	char d = 0;

	CASE("dtmf parse: a signal and a duration");
	CHECK(dtmf_relay_parse("Signal=5\r\nDuration=160\r\n", &d, &ms));
	CHECK(d == '5');
	CHECK_UINT(ms, 160);

	CASE("dtmf parse: the colon spelling of both keywords");
	CHECK(dtmf_relay_parse("Signal: 7\r\nDuration: 90\r\n", &d, &ms));
	CHECK(d == '7');
	CHECK_UINT(ms, 90);

	CASE("dtmf parse: a missing duration reads as none");
	CHECK(dtmf_relay_parse("Signal=3\r\n", &d, &ms));
	CHECK(d == '3');
	CHECK_UINT(ms, 0);

	CASE("dtmf parse: a body with no terminator at all");
	CHECK(dtmf_relay_parse("Signal=9", &d, &ms));
	CHECK(d == '9');
	CHECK_UINT(ms, 0);

	CASE("dtmf parse: the two symbol keys");
	CHECK(dtmf_relay_parse("Signal=*\r\n", &d, &ms));
	CHECK(d == '*');
	CHECK(dtmf_relay_parse("Signal=#\r\n", &d, &ms));
	CHECK(d == '#');

	CASE("dtmf parse: a lower case letter key is folded up");
	CHECK(dtmf_relay_parse("Signal: a\r\n", &d, &ms));
	CHECK(d == 'A');
	CHECK(dtmf_relay_parse("Signal=d\r\n", &d, &ms));
	CHECK(d == 'D');

	CASE("dtmf parse: an upper case letter key travels as it is");
	CHECK(dtmf_relay_parse("Signal=B\r\n", &d, &ms));
	CHECK(d == 'B');

	/* RFC 4733 section 3.2 numbers the same keys */
	CASE("dtmf parse: the event codes of the letters and the symbols");
	CHECK(dtmf_relay_parse("Signal=10\r\n", &d, &ms));
	CHECK(d == '*');
	CHECK(dtmf_relay_parse("Signal=11\r\n", &d, &ms));
	CHECK(d == '#');
	CHECK(dtmf_relay_parse("Signal=12\r\n", &d, &ms));
	CHECK(d == 'A');
	CHECK(dtmf_relay_parse("Signal=15\r\n", &d, &ms));
	CHECK(d == 'D');

	CASE("dtmf parse: an event code of a plain digit is that digit");
	CHECK(dtmf_relay_parse("Signal=00\r\n", &d, &ms));
	CHECK(d == '0');

	/*
	 * Code 16 is the hook flash of RFC 4733 and the codes above it are
	 * tones no subscriber line plays, so the whole range goes.
	 */
	CASE("dtmf parse: an event code above the key range is refused");
	d = 0;
	CHECK(!dtmf_relay_parse("Signal=16\r\n", &d, &ms));
	CHECK(d == 0);
	CHECK(!dtmf_relay_parse("Signal=99\r\n", &d, &ms));

	CASE("dtmf parse: the flash key of the BroadWorks spelling is refused");
	CHECK(!dtmf_relay_parse("Signal=!\r\n", &d, &ms));

	CASE("dtmf parse: a signal outside the alphabet is refused");
	CHECK(!dtmf_relay_parse("Signal=Z\r\n", &d, &ms));
	CHECK(!dtmf_relay_parse("Signal=\r\n", &d, &ms));
	CHECK(!dtmf_relay_parse("Signal=55\r\n", &d, &ms));

	CASE("dtmf parse: trailing text on the signal line refuses the body");
	CHECK(!dtmf_relay_parse("Signal=5 and more\r\n", &d, &ms));

	CASE("dtmf parse: a missing separator refuses the body");
	CHECK(!dtmf_relay_parse("Signal 5\r\n", &d, &ms));

	CASE("dtmf parse: a body with no signal at all is refused");
	CHECK(!dtmf_relay_parse("Duration=160\r\n", &d, &ms));
	CHECK(!dtmf_relay_parse("", &d, &ms));
	CHECK(!dtmf_relay_parse("ring the bell\r\n", &d, &ms));

	CASE("dtmf parse: a refused body leaves both outputs at zero");
	d = '5';
	ms = 160;
	CHECK(!dtmf_relay_parse("ring the bell\r\n", &d, &ms));
	CHECK(d == 0);
	CHECK_UINT(ms, 0);

	CASE("dtmf parse: an oversized duration saturates");
	CHECK(dtmf_relay_parse("Signal=1\r\nDuration=99999\r\n", &d, &ms));
	CHECK_UINT(ms, 65535);

	CASE("dtmf parse: a duration with no number reads as none");
	CHECK(dtmf_relay_parse("Signal=1\r\nDuration=\r\n", &d, &ms));
	CHECK_UINT(ms, 0);

	CASE("dtmf parse: the duration may lead the signal");
	CHECK(dtmf_relay_parse("Duration=40\r\nSignal=2\r\n", &d, &ms));
	CHECK(d == '2');
	CHECK_UINT(ms, 40);

	CASE("dtmf parse: the bare body of application/dtmf");
	CHECK(dtmf_parse("application/dtmf", "7", &d, &ms));
	CHECK(d == '7');
	CHECK_UINT(ms, 0);

	CASE("dtmf parse: the bare body skips the whitespace in front of it");
	CHECK(dtmf_parse("application/dtmf", "\r\n c\r\n", &d, &ms));
	CHECK(d == 'C');

	CASE("dtmf parse: an empty bare body is refused");
	CHECK(!dtmf_parse("application/dtmf", "", &d, &ms));
	CHECK(!dtmf_parse("application/dtmf", "  \r\n", &d, &ms));

	CASE("dtmf parse: a bare body outside the alphabet is refused");
	CHECK(!dtmf_parse("application/dtmf", "Z\r\n", &d, &ms));

	/* the bare body is one key, so a two digit event code is not one */
	CASE("dtmf parse: a bare body reads one byte and nothing more");
	CHECK(dtmf_parse("application/dtmf", "10", &d, &ms));
	CHECK(d == '1');

	CASE("dtmf parse: a content type neither arm owns is refused");
	CHECK(!dtmf_parse("application/broadsoft", "Signal=5\r\n", &d, &ms));

	CASE("dtmf ctype: both media types match");
	CHECK(call_dtmf_relay_ctype_match(sip_str_cstr("application/dtmf-relay")));
	CHECK(call_dtmf_relay_ctype_match(sip_str_cstr("application/dtmf")));

	CASE("dtmf ctype: the media type is matched case insensitively");
	CHECK(call_dtmf_relay_ctype_match(sip_str_cstr("Application/DTMF-Relay")));

	CASE("dtmf ctype: parameters and surrounding whitespace are tolerated");
	CHECK(call_dtmf_relay_ctype_match(
		sip_str_cstr("application/dtmf-relay;charset=utf-8")));
	CHECK(call_dtmf_relay_ctype_match(sip_str_cstr(" application/dtmf ")));

	CASE("dtmf ctype: every other media type is refused");
	CHECK(!call_dtmf_relay_ctype_match(sip_str_cstr("application/broadsoft")));
	CHECK(!call_dtmf_relay_ctype_match(sip_str_cstr("application/dtmf-relayx")));
	CHECK(!call_dtmf_relay_ctype_match(sip_str_cstr("")));

	/* the two parsers own two bodies, and neither reads the other's */
	CASE("dtmf ctype: the call waiting matcher still refuses both of them");
	CHECK(!call_cw_ctype_match(sip_str_cstr("application/dtmf-relay")));
	CHECK(!call_cw_ctype_match(sip_str_cstr("application/dtmf")));
}

static void test_dtmf_wire(void)
{
	unsigned int before;

	fx_reset();
	uas_up("call-dtmf1");

	CASE("dtmf info: a signal and a duration reach the FSM");
	before = fk.sends;
	rx(uas_in_dialog("INFO", 2, DTMF_RELAY_CTYPE, "Signal=5\r\nDuration=160\r\n"));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(ev.digit_net, 1);
	CHECK(ev.digit_net_digit == '5');
	CHECK_UINT(ev.digit_net_ms, 160);
	CHECK_UINT(ev.digit_net_line, 0);
	CHECK_UINT(ev.digit_net_slot, 0);

	/* the answer is decided by the content type alone, so a shell that
	 * drops the digit downstream never changes what the switch sees */
	CASE("dtmf info: the 200 is on the wire before the digit is dispatched");
	CHECK_UINT(ev.digit_net_sends, before + 1);

	CASE("dtmf info: the pound key travels");
	rx(uas_in_dialog("INFO", 3, DTMF_RELAY_CTYPE, "Signal=#\r\n"));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(ev.digit_net, 2);
	CHECK(ev.digit_net_digit == '#');
	CHECK_UINT(ev.digit_net_ms, 0);

	CASE("dtmf info: the event code of the star key travels");
	rx(uas_in_dialog("INFO", 4, DTMF_RELAY_CTYPE, "Signal=10\r\nDuration=80\r\n"));
	CHECK_UINT(ev.digit_net, 3);
	CHECK(ev.digit_net_digit == '*');
	CHECK_UINT(ev.digit_net_ms, 80);

	CASE("dtmf info: the event code of the pound key travels");
	rx(uas_in_dialog("INFO", 5, DTMF_RELAY_CTYPE, "Signal=11\r\n"));
	CHECK_UINT(ev.digit_net, 4);
	CHECK(ev.digit_net_digit == '#');

	CASE("dtmf info: a lower case letter key is folded up");
	rx(uas_in_dialog("INFO", 6, DTMF_RELAY_CTYPE, "Signal: a\r\n"));
	CHECK_UINT(ev.digit_net, 5);
	CHECK(ev.digit_net_digit == 'A');

	CASE("dtmf info: the bare body of application/dtmf travels");
	rx(uas_in_dialog("INFO", 7, DTMF_CTYPE, "7"));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(ev.digit_net, 6);
	CHECK(ev.digit_net_digit == '7');
	CHECK_UINT(ev.digit_net_ms, 0);

	/*
	 * A dtmf-relay INFO is never answered 400, whatever the body carries:
	 * the far end owns the digit, and a call is worth more than a body
	 * this parser cannot read.
	 */
	CASE("dtmf info: an unreadable body keeps the 200 and dispatches nothing");
	rx(uas_in_dialog("INFO", 8, DTMF_RELAY_CTYPE, "Signal=!\r\n"));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(ev.digit_net, 6);

	CASE("dtmf info: garbage in the body is answered the same way");
	rx(uas_in_dialog("INFO", 9, DTMF_RELAY_CTYPE, "ring the bell\r\n"));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(ev.digit_net, 6);

	CASE("dtmf info: a body-less dtmf-relay INFO is answered and silent");
	rx(uas_in_dialog("INFO", 10, DTMF_RELAY_CTYPE, NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(ev.digit_net, 6);

	CASE("dtmf info: the digit does not touch the media or the slot");
	CHECK_UINT(med.stop_calls, 0);
	CHECK(slot_is(1));

	CASE("dtmf info: the call waiting report is untouched by any of them");
	CHECK_UINT(ev.cw_net, 0);

	/* the leg is driven directly, for the reason the call waiting case
	 * beside it gives: the account intake answers a NOTIFY 489 itself */
	CASE("dtmf info: another method with the same body carries no digit");
	rx_in_dialog_direct(0, uas_in_dialog("NOTIFY", 11, DTMF_RELAY_CTYPE,
					     "Signal=5\r\n"));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(ev.digit_net, 6);

	CASE("dtmf info: an INFO for a dialog that does not exist is refused 481");
	fx_reset();
	rx(dialog_req_text("INFO", 9, "no-such-dialog", "peer", "us",
			   DTMF_RELAY_CTYPE, "Signal=5\r\n"));
	CHECK(has(last_text(), "SIP/2.0 481"));
	CHECK_UINT(ev.digit_net, 0);
}

/* ------------------------------------------- the local call forward 302 */

/*
 * The redirect is the only mechanism. The offered call is answered 302 with
 * the target in its Contact, and the leg ends. This line therefore never
 * rings and originates no call of its own.
 */
static void test_forward(void)
{
	char tag[64];

	fx_reset();

	CASE("forward: a free slot has no offered call to redirect");
	CHECK(!call_forward(0, 0, "5551234"));
	CHECK_UINT(fk.sends, 0);

	CASE("forward: an offered call answers 302");
	fx_reset();
	rx(invite_text("call-f1", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK_UINT(ev.incoming, 1);
	CHECK(call_forward(0, 0, "5551234"));
	CHECK(has(last_text(), "SIP/2.0 302"));

	CASE("forward: the Contact carries the target at the account domain");
	CHECK(has(last_text(), "Contact: <sip:5551234@" T_DOMAIN ">"));

	CASE("forward: no Diversion header");
	CHECK(!has(last_text(), "Diversion"));

	/*
	 * A target that already names a URI is used verbatim. A ':' or an '@'
	 * says so, and neither can be dialled, so a collected target never
	 * takes this branch; a configured call_fwd_*_uri may. Wrapping such a
	 * value would build a Contact no proxy can route.
	 */
	CASE("forward: a full sip URI target is used verbatim");
	fx_reset();
	rx(invite_text("call-f5", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(call_forward(0, 0, "sip:bob@example.net"));
	CHECK(has(last_text(), "Contact: <sip:bob@example.net>"));
	/* the wrapping build would have produced sip:sip:bob@... @our domain */
	CHECK(!has(last_text(), "sip:sip:"));

	CASE("forward: a tel URI target is used verbatim too");
	fx_reset();
	rx(invite_text("call-f6", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(call_forward(0, 0, "tel:+15555550123"));
	CHECK(has(last_text(), "Contact: <tel:+15555550123>"));

	CASE("forward: a target that carries a host alone is verbatim as well");
	fx_reset();
	rx(invite_text("call-f7", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(call_forward(0, 0, "bob@example.net"));
	CHECK(has(last_text(), "Contact: <bob@example.net>"));

	CASE("forward: a bare number still takes the account domain");
	fx_reset();
	rx(invite_text("call-f8", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(call_forward(0, 0, "5551234"));
	CHECK(has(last_text(), "Contact: <sip:5551234@" T_DOMAIN ">"));

	CASE("forward: the final response carries a To tag of its own");
	CHECK(resp_to_tag(fk.sends - 1, tag, sizeof(tag)));
	CHECK(tag[0] != 0);

	CASE("forward: the body is empty");
	CHECK(has(last_text(), "Content-Length: 0\r\n"));

	CASE("forward: the leg ends and reports no failure to the FSM");
	CHECK(slot_is(0));
	CHECK_UINT(ev.failed, 0);

	CASE("forward: a second redirect on the freed slot is refused");
	CHECK(!call_forward(0, 0, "5551234"));

	CASE("forward: an empty target is refused and nothing is sent");
	fx_reset();
	rx(invite_text("call-f2", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(!call_forward(0, 0, ""));
	CHECK(!call_forward(0, 0, NULL));
	CHECK(!has(last_text(), "SIP/2.0 302"));
	CHECK(slot_is(1));

	CASE("forward: a leg that already rang keeps the tag it used");
	fx_reset();
	rx(invite_text("call-f3", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(call_ring(0, 0));
	CHECK(resp_to_tag(fk.sends - 1, tag, sizeof(tag)));
	CHECK(call_forward(0, 0, "5551234"));
	CHECK(has(last_text(), "SIP/2.0 302"));
	CHECK(has(last_text(), tag));

	CASE("forward: an answered call is no longer an offer");
	fx_reset();
	rx(invite_text("call-f4", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(call_answer(0, 0));
	CHECK(!call_forward(0, 0, "5551234"));

	/*
	 * A target the redirect cannot build leaves the offer alone. The
	 * resolution runs before the leg is committed, so a refusal costs the
	 * caller nothing and it keeps being served: ending the leg for a
	 * Contact this layer could not write would drop the call instead. No
	 * configured target reaches it today, and the order is what keeps that
	 * true if one ever grows.
	 */
	CASE("forward: a target too wide for a Contact leaves the offer unforwarded");
	fx_reset();
	rx(invite_text("call-f9", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK_UINT(ev.incoming, 1);
	{
		char big[512];

		memset(big, '9', sizeof(big) - 1);
		big[sizeof(big) - 1] = 0;
		CHECK(!call_forward(0, 0, big));
	}
	CHECK(find_send("SIP/2.0 302") < 0);
	CHECK_UINT(ev.ended, 0);
	CHECK(slot_is(1));

	CASE("forward: so the offered call is still there to answer or redirect");
	CHECK(call_forward(0, 0, "5551234"));
	CHECK(has(last_text(), "SIP/2.0 302"));
	CHECK(has(last_text(), "Contact: <sip:5551234@" T_DOMAIN ">"));
	CHECK_UINT(ev.ended, 1);

	CASE("forward: a slot out of range is refused like every other command");
	CHECK(!call_forward(0, CALL_SLOTS, "5551234"));
	CHECK(!call_forward(CFG_CHANNEL_MAX, 0, "5551234"));
}

/* ------------------------------------------ the relayed hook flash INFO */

#define FLASH_BODY	"event flashhook\r\n"

static void test_flash_relay(void)
{
	struct req_info ri, inv;
	unsigned int sends, first_info;

	fx_reset();

	CASE("flash relay: a free slot has no dialog to relay on");
	CHECK(!call_flash_relay(0, 0));
	CHECK(find_send("INFO") < 0);

	CASE("flash relay: an early leg refuses it, the dialog is not confirmed yet");
	CHECK(call_dial(0, 0, "1002", true));
	reply(100, NULL, NULL, NULL);
	reply(180, "peer-tag", NULL, NULL);
	CHECK(!call_flash_relay(0, 0));
	CHECK(find_send("INFO") < 0);

	CASE("flash relay: a slot out of range is refused like every other command");
	CHECK(!call_flash_relay(0, CALL_SLOTS));
	CHECK(!call_flash_relay(CFG_CHANNEL_MAX, 0));

	CASE("flash relay: a connected leg puts exactly one INFO on the wire");
	fx_reset();
	uac_up(NULL);
	CHECK(req_of("INVITE", &inv));
	sends = fk.sends;
	CHECK(call_flash_relay(0, 0));
	CHECK_UINT(fk.sends, sends + 1);
	CHECK_UINT(count_send("INFO"), 1);
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.method, "INFO"));

	CASE("flash relay: the media type is application/broadsoft");
	CHECK(has(last_text(), "Content-Type: application/broadsoft\r\n"));

	CASE("flash relay: the body is the flash event line and nothing else");
	CHECK_UINT(ri.body_len, strlen(FLASH_BODY));
	CHECK(!strcmp(ri.body, FLASH_BODY));
	CHECK(has(last_text(), "Content-Length: 17\r\n"));

	CASE("flash relay: it rides the in-dialog identity of the leg");
	CHECK(!strcmp(ri.call_id, inv.call_id));
	CHECK(!strcmp(ri.from_tag, inv.from_tag));
	CHECK(!strcmp(ri.to_tag, "peer-tag"));
	CHECK(!strcmp(ri.ruri, "sip:1002@" T_PEER ":5060"));

	CASE("flash relay: the CSeq advances past the INVITE");
	CHECK(ri.cseq > inv.cseq);

	/*
	 * Fire and forget. The far end owns the feature, so its refusal ends at
	 * the trace: no failure reaches the FSM, the media keeps running and
	 * the leg stays in the call.
	 */
	CASE("flash relay: a 4xx answer is absorbed and traced");
	reply(488, "peer-tag", NULL, NULL);
	CHECK_UINT(tr.flash_lines, 1);
	CHECK(has(tr.flash, "flash INFO refused 488"));
	CHECK_UINT(ev.failed, 0);
	CHECK_UINT(med.stop_calls, 0);
	CHECK(find_send("BYE") < 0);
	CHECK(slot_is(1));

	CASE("flash relay: the call keeps running after the refusal");
	CHECK(call_dtmf(0, 0, '5', 100, false));
	CHECK_UINT(med.dtmf_calls, 1);

	CASE("flash relay: a second flash is a second INFO on the next CSeq");
	CHECK(call_flash_relay(0, 0));
	CHECK_UINT(count_send("INFO"), 2);
	CHECK(last_req(&inv));
	CHECK(inv.cseq == ri.cseq + 1);

	CASE("flash relay: a 200 answer is absorbed without a word");
	reply(200, "peer-tag", NULL, NULL);
	CHECK_UINT(tr.flash_lines, 1);
	CHECK_UINT(ev.failed, 0);
	CHECK(slot_is(1));

	/*
	 * Two flashes with the first answer still outstanding. The handle holds
	 * one relay, so the newer INFO takes it and the older transaction runs
	 * to its own end: both requests leave, the leg is untouched by either
	 * answer, and the trace is the only thing the supersede costs.
	 */
	CASE("flash relay: a second flash while the first is outstanding says so");
	fx_reset();
	uac_up(NULL);
	first_info = fk.sends;
	CHECK(call_flash_relay(0, 0));
	CHECK(call_flash_relay(0, 0));
	CHECK_UINT(count_send("INFO"), 2);
	CHECK_UINT(tr.flash_lines, 1);
	CHECK(has(tr.flash, "flash INFO superseded by a newer one"));
	CHECK(slot_is(1));

	CASE("flash relay: the superseded final is absorbed without a word");
	reply_at(first_info, 603, "peer-tag", NULL, NULL);
	CHECK_UINT(tr.flash_lines, 1);
	CHECK_UINT(ev.failed, 0);
	CHECK_UINT(med.stop_calls, 0);
	CHECK(find_send("BYE") < 0);
	CHECK(slot_is(1));

	CASE("flash relay: a refusal of the newer INFO still reaches the trace");
	reply(486, "peer-tag", NULL, NULL);
	CHECK_UINT(tr.flash_lines, 2);
	CHECK(has(tr.flash, "flash INFO refused 486"));
	CHECK_UINT(ev.failed, 0);
	CHECK(slot_is(1));

	/* the leg carries the anonymous From of the call it dialled, so the
	 * relay is withheld exactly like the INVITE was */
	CASE("flash relay: a withheld call relays under its anonymous identity");
	fx_reset();
	CHECK(dial_present(0, 0, "1002", false));
	reply(100, NULL, NULL, NULL);
	reply(200, "peer-tag", NULL, sdp_body(T_PEER, 20000, "0 101", NULL));
	CHECK(call_flash_relay(0, 0));
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.method, "INFO"));
	CHECK(has(ri.from, "sip:anonymous@anonymous.invalid"));

	CASE("flash relay: a leg that is clearing has nothing to relay on");
	fx_reset();
	uac_up(NULL);
	CHECK(call_hangup(0, 0));
	CHECK(!call_flash_relay(0, 0));
	CHECK(find_send("INFO") < 0);
}

/* --------------------------------------------------- the transferor's REFER */

/*
 * The transferor side of RFC 3515 (call.h call_xfer and call_xfer_consult).
 * Every case drives the leg layer alone: line/line.c owns the star codes, the
 * FSM owns the states, and what is asserted here is the wire, the one outcome
 * and what the leg does to itself around it.
 */

/*
 * RFC 3261 25.1, the escape uri_header_escape() applies to an embedded URI
 * header. The suite computes it itself, so the byte pin of a Refer-To is not
 * the implementation answering its own question.
 */
static const char *xfer_escape(const char *s)
{
	static const char plain[] = "-_.!~*'()[]/?:+$";
	char *buf = text_slot();
	size_t n = 0;

	for (; *s && n + 4 < TEXT_MAX; s++) {
		if (isalnum((unsigned char)*s) || strchr(plain, *s)) {
			buf[n++] = *s;
			continue;
		}

		n += (size_t)snprintf(buf + n, TEXT_MAX - n, "%%%02X", (unsigned char)*s);
	}
	buf[n] = 0;

	return buf;
}

/*
 * One NOTIFY of the implicit subscription, as the transferee sends it: the
 * Event package with the id of our REFER, the subscription state and a
 * message/sipfrag body carrying the status line that is the verdict.
 */
static const char *xfer_notify_text(unsigned int cseq, const char *event,
				    const char *sub_state, const char *frag)
{
	char extra[256];

	snprintf(extra, sizeof(extra),
		 "Event: %s\r\n"
		 "Subscription-State: %s\r\n"
		 "Content-Type: message/sipfrag;version=2.0\r\n",
		 event, sub_state);

	return uac_in_dialog("NOTIFY", "peer-tag", cseq, extra, frag);
}

/* the same NOTIFY with the id our REFER handed out, which is its CSeq */
static const char *xfer_notify(unsigned int cseq, unsigned int id, const char *sub_state,
			       const char *frag)
{
	char event[32];

	snprintf(event, sizeof(event), "refer;id=%u", id);

	return xfer_notify_text(cseq, event, sub_state, frag);
}

/* an answered outbound leg with a transfer of the given kind accepted on it */
static void xfer_up(enum call_xfer_kind kind)
{
	fx_reset();
	uac_up(NULL);
	call_xfer(0, 0, kind, "1003");
	reply(202, NULL, NULL, NULL);
}

static void test_xfer_refusals(void)
{
	struct req_info ri;
	/* wider than any request URI this layer builds, which is call.c's own
	 * CALL_URI_MAX and is not exported */
	char big[512];

	CASE("xfer: a free slot has no leg to refer on");
	fx_reset();
	CHECK(!call_xfer(0, 0, CALL_XFER_BLIND, "1003"));
	CHECK_UINT(fk.sends, 0);
	CHECK_UINT(ev.xfer_done, 0);

	CASE("xfer: a slot out of range is refused like every other command");
	CHECK(!call_xfer(0, CALL_SLOTS, CALL_XFER_BLIND, "1003"));
	CHECK(!call_xfer(CFG_CHANNEL_MAX, 0, CALL_XFER_BLIND, "1003"));
	CHECK(!call_xfer_consult(CFG_CHANNEL_MAX, 0, 1));
	CHECK_UINT(fk.sends, 0);

	CASE("xfer: an empty target is refused and nothing goes out");
	fx_reset();
	uac_up(NULL);
	CHECK(!call_xfer(0, 0, CALL_XFER_BLIND, ""));
	CHECK(!call_xfer(0, 0, CALL_XFER_BLIND, NULL));
	CHECK(find_send("REFER") < 0);
	CHECK_UINT(ev.xfer_done, 0);

	/* the refuse-not-truncate rule of this layer, at the one buffer a
	 * caller fills: half a target names a party nobody meant */
	CASE("xfer: a target too long for a request URI is refused, not truncated");
	memset(big, '9', sizeof(big) - 1);
	big[sizeof(big) - 1] = 0;
	CHECK(!call_xfer(0, 0, CALL_XFER_BLIND, big));
	CHECK(find_send("REFER") < 0);
	CHECK_UINT(ev.xfer_done, 0);

	CASE("xfer: and the leg still takes an ordinary target after it");
	CHECK(call_xfer(0, 0, CALL_XFER_BLIND, "1003"));
	CHECK(req_of("REFER", &ri));
	CHECK(has(ri.ruri, "sip:1002@" T_PEER));

	CASE("xfer: a leg that already carries an outstanding REFER refuses a second");
	CHECK(!call_xfer(0, 0, CALL_XFER_BLIND, "1004"));
	CHECK_UINT(count_send("REFER"), 1);

	CASE("xfer: an early leg has no confirmed dialog to refer on");
	fx_reset();
	CHECK(dial(0, 0, "1002"));
	reply(100, NULL, NULL, NULL);
	reply(180, "peer-tag", NULL, NULL);
	CHECK(!call_xfer(0, 0, CALL_XFER_BLIND, "1003"));
	CHECK(find_send("REFER") < 0);

	CASE("xfer: an offered inbound leg has none either");
	fx_reset();
	rx(invite_text("call-x1", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(call_ring(0, 0));
	CHECK(!call_xfer(0, 0, CALL_XFER_BLIND, "1003"));
	CHECK(find_send("REFER") < 0);

	CASE("xfer: an answered inbound leg waiting for its ACK is refused too");
	CHECK(call_answer(0, 0));
	CHECK(!call_xfer(0, 0, CALL_XFER_BLIND, "1003"));
	CHECK(find_send("REFER") < 0);

	CASE("xfer: and takes the transfer once the ACK confirms the dialog");
	rx(uas_in_dialog("ACK", 1, NULL, NULL));
	CHECK(call_xfer(0, 0, CALL_XFER_BLIND, "1003"));
	CHECK(find_send("REFER") >= 0);

	CASE("xfer: a leg that is clearing has nothing to refer on");
	fx_reset();
	uac_up(NULL);
	CHECK(call_hangup(0, 0));
	CHECK(!call_xfer(0, 0, CALL_XFER_BLIND, "1003"));
	CHECK(find_send("REFER") < 0);
	CHECK_UINT(ev.xfer_done, 0);

	CASE("xfer: a draining leg the UI already gave up is refused");
	reply(200, NULL, NULL, NULL);
	fake_advance(6000);
	CHECK(!call_xfer(0, 0, CALL_XFER_BLIND, "1003"));
	CHECK(find_send("REFER") < 0);

	CASE("xfer: a refused request makes no callback at all");
	CHECK_UINT(ev.xfer_done, 0);
	CHECK_UINT(ev.failed, 0);

	/*
	 * A refusal leaves no subscription behind either. false means no wire
	 * exchange happened, so nothing owes an outcome and nothing is
	 * outstanding: a leg that kept the pending state of a request it never
	 * sent would refuse every later transfer for the rest of the call and
	 * would consume a NOTIFY that names nothing.
	 */
	CASE("xfer: a REFER that could not be sent leaves no transfer outstanding");
	fx_reset();
	uac_up(NULL);
	fk.send_fail = true;
	CHECK(!call_xfer(0, 0, CALL_XFER_ATTENDED, "1003"));
	fk.send_fail = false;
	CHECK_UINT(count_send("REFER"), 0);
	CHECK_UINT(ev.xfer_done, 0);

	CASE("xfer: so a refer NOTIFY still names no outstanding transfer");
	rx(xfer_notify(2, 2, "active;expires=60", "SIP/2.0 200 OK\r\n"));
	CHECK(has(last_text(), "SIP/2.0 481"));
	CHECK_UINT(ev.xfer_done, 0);

	CASE("xfer: and the leg takes the next transfer it is given");
	CHECK(call_xfer(0, 0, CALL_XFER_ATTENDED, "1003"));
	CHECK_UINT(count_send("REFER"), 1);
	reply(202, NULL, NULL, NULL);
	CHECK(timer_armed(SIP_TIMER_CALL_C));
	CHECK_UINT(ev.xfer_done, 0);
}

static void test_xfer_blind(void)
{
	struct req_info ri, inv;
	struct slot_view sv;
	uint64_t accept_at;

	fx_reset();
	uac_up(NULL);
	CHECK(req_of("INVITE", &inv));

	CASE("blind: the transfer puts exactly one REFER on the wire");
	CHECK(call_xfer(0, 0, CALL_XFER_BLIND, "1003"));
	CHECK_UINT(count_send("REFER"), 1);
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.method, "REFER"));

	CASE("blind: it rides the in-dialog identity of the leg");
	CHECK(!strcmp(ri.call_id, inv.call_id));
	CHECK(!strcmp(ri.from_tag, inv.from_tag));
	CHECK(!strcmp(ri.to_tag, "peer-tag"));
	CHECK(!strcmp(ri.ruri, "sip:1002@" T_PEER ":5060"));

	CASE("blind: on the next CSeq of the dialog");
	CHECK_UINT(ri.cseq, inv.cseq + 1);

	CASE("blind: the Refer-To names the target in the account domain");
	CHECK(has(last_text(), "Refer-To: <sip:1003@" T_DOMAIN ">\r\n"));

	CASE("blind: and carries no Replaces, which only a consult transfer has");
	CHECK(!has(last_text(), "Replaces"));

	CASE("blind: Referred-By carries our own identity (RFC 3892 5)");
	CHECK(has(last_text(), "Referred-By: <sip:1001@" T_DOMAIN ">\r\n"));

	CASE("blind: and Contact rides the REFER (RFC 3515 2.4)");
	CHECK(has(last_text(), "Contact: <sip:1001@" T_HOST ":5060>\r\n"));

	CASE("blind: the request carries no body");
	CHECK(has(last_text(), "Content-Length: 0\r\n"));
	CHECK_UINT(ri.body_len, 0);

	CASE("blind: nothing about the leg moved before the answer");
	CHECK_UINT(ev.xfer_done, 0);
	CHECK_UINT(med.stop_calls, 0);
	CHECK(find_send("BYE") < 0);
	CHECK(slot_is(1));

	CASE("blind: a target that already names a URI is used verbatim");
	fx_reset();
	uac_up(NULL);
	CHECK(call_xfer(0, 0, CALL_XFER_BLIND, "sip:bob@example.net"));
	CHECK(has(last_text(), "Refer-To: <sip:bob@example.net>\r\n"));
	CHECK(!has(last_text(), "sip:sip:"));

	CASE("blind: the 202 reports the transfer done and silences the leg");
	fx_reset();
	uac_up(NULL);
	CHECK(call_xfer(0, 0, CALL_XFER_BLIND, "1003"));
	accept_at = fk.now;
	reply(202, NULL, NULL, NULL);
	CHECK_UINT(ev.xfer_done, 1);
	CHECK(ev.xfer_ok);
	CHECK_UINT(ev.xfer_code, 202);
	CHECK_UINT(ev.xfer_line, 0);
	CHECK_UINT(ev.xfer_slot, 0);
	CHECK(find_send("BYE") < 0);

	/* call.h: the leg holds no UI name and no occupancy by the time the
	 * outcome fires, and the FSM owns the slot roles from that instant */
	CASE("blind: no BYE was on the wire when the outcome was reported");
	CHECK_UINT(ev.xfer_byes, 0);
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.state, "active"));
	CHECK(!sv.ui_busy);
	CHECK(sv.shadow);

	CASE("blind: the stream stopped with the silence");
	CHECK_UINT(med.stop_calls, 1);
	CHECK_UINT(med.snapshot_calls, 1);

	CASE("blind: the drain is bounded by the transfer guard");
	CHECK(timer_armed(SIP_TIMER_CALL_C));
	CHECK_UINT(timer_delay(SIP_TIMER_CALL_C), CALL_XFER_WAIT_MS);

	CASE("blind: no clearing class and no other FSM event follow the ok");
	CHECK_UINT(ev.failed, 0);
	CHECK_UINT(ev.remote_bye, 0);
	CHECK_UINT(ev.ended, 0);

	CASE("blind: the other slot is free while the drain holds this one");
	CHECK(slot_is(1));

	/*
	 * The switch order of a Metaswitch: a progress NOTIFY, a
	 * c=0.0.0.0 re-INVITE on the referred leg, the final NOTIFY, and the
	 * switch's own BYE behind it. The drained leg answers all four. A BYE
	 * at the 202 would meet them with 481 and 491.
	 */
	CASE("drain: the switch's progress NOTIFY on the drained leg is answered 200");
	fake_advance(500);
	rx(xfer_notify(2, 2, "active;expires=3600", "SIP/2.0 100 Trying\r\n"));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(find_send("BYE") < 0);

	CASE("drain: its c=0.0.0.0 re-INVITE is answered 200 inactive with no stream and no session timer");
	{
		unsigned int starts = med.start_calls, configs = med.config_calls;

		rx(uac_in_dialog("INVITE", "peer-tag", 3, NULL,
				 sdp_body("0.0.0.0", 20010, "0", NULL)));
		CHECK(has(last_text(), "SIP/2.0 200"));
		CHECK(has(last_text(), "a=inactive"));
		CHECK(!has(last_text(), "Session-Expires"));
		rx(uac_in_dialog("ACK", "peer-tag", 3, NULL, NULL));
		CHECK_UINT(med.start_calls, starts);
		CHECK_UINT(med.config_calls, configs);
	}

	CASE("drain: the final NOTIFY is answered 200 and the verdict arms the BYE grace");
	{
		unsigned int answered = fk.sends;

		rx(xfer_notify(4, 2, "terminated;reason=timeout", "SIP/2.0 200 OK\r\n"));
		CHECK(has(send_text(answered), "SIP/2.0 200"));
		CHECK(find_send("BYE") < 0);
		CHECK_UINT(ev.xfer_done, 1);
		CHECK_UINT((unsigned int)timer_delay(SIP_TIMER_CALL_C), CALL_XFER_BYE_GRACE_MS);
		CHECK_UINT((unsigned int)timer_delay(SIP_TIMER_CALL_C), 1000);
	}

	/*
	 * The switch's BYE 1 ms behind its final NOTIFY (call.h: an ok outcome
	 * is the last word on that slot). The FSM has already given the slot
	 * another role, so no far-end clear may reach it. The BYE is answered,
	 * ends the leg, and cancels the grace: nothing of ours follows.
	 */
	CASE("drain: the switch's BYE behind its NOTIFY is answered 200 and reported nowhere");
	fake_advance(1);
	rx(uac_in_dialog("BYE", "peer-tag", 5, NULL, NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(ev.remote_bye, 0);
	CHECK_UINT(ev.failed, 0);
	CHECK(find_send("BYE") < 0);
	CHECK(!timer_armed(SIP_TIMER_CALL_C));
	fake_advance(CALL_XFER_BYE_GRACE_MS);
	CHECK(find_send("BYE") < 0);

	CASE("drain: the record of that leg says a transfer ended it, dated at the 202");
	CHECK_UINT(ev.ended, 1);
	CHECK(ev.end.transferred);
	CHECK(ev.end.answered);
	CHECK(!ev.end.failed);
	CHECK(ev.end.cleared);
	CHECK_UINT(ev.end.at_ms - ev.end.cleared_ago_ms, accept_at);
	CHECK_UINT(ev.xfer_done, 1);
	CHECK(slot_is(0));

	CASE("drain: a retransmitted NOTIFY behind the end keeps its cached answer");
	rx(xfer_notify(4, 2, "terminated;reason=timeout", "SIP/2.0 200 OK\r\n"));
	CHECK(has(last_text(), "SIP/2.0 200"));

	CASE("drain: a new NOTIFY behind the end meets no subscription, 481");
	rx(xfer_notify(6, 2, "terminated;reason=noresource", "SIP/2.0 200 OK\r\n"));
	CHECK(has(last_text(), "SIP/2.0 481"));
	CHECK_UINT(ev.xfer_done, 1);

	CASE("crossing bye: the attended outcome takes the peer BYE the same way");
	xfer_up(CALL_XFER_ATTENDED);
	rx(xfer_notify(2, 2, "terminated", "SIP/2.0 200 OK\r\n"));
	CHECK_UINT(ev.xfer_done, 1);
	CHECK(ev.xfer_ok);
	rx(uac_in_dialog("BYE", "peer-tag", 3, NULL, NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(ev.remote_bye, 0);
	CHECK_UINT(ev.failed, 0);
	CHECK_UINT(ev.ended, 1);
	CHECK(ev.end.transferred);

	/* the suppression is scoped to a leg the transfer cleared: an ordinary
	 * call still reports a far-end BYE, and a peer BYE that crosses a plain
	 * hangup keeps whatever it always did */
	CASE("crossing bye: an ordinary leg still reports the far-end clear");
	fx_reset();
	uac_up(NULL);
	rx(uac_in_dialog("BYE", "peer-tag", 2, NULL, NULL));
	CHECK_UINT(ev.remote_bye, 1);
	CHECK_UINT(ev.ended, 1);
	CHECK(!ev.end.transferred);

	/* a transferee that follows RFC 5589 6.1 sends no BYE of its own: this
	 * side owes the BYE, and sends it once the grace ran out */
	CASE("drain: with no network BYE the grace ends the leg with this side's BYE");
	fx_reset();
	uac_up(NULL);
	CHECK(call_xfer(0, 0, CALL_XFER_BLIND, "1003"));
	reply(202, NULL, NULL, NULL);
	rx(xfer_notify(2, 2, "terminated;reason=noresource", "SIP/2.0 200 OK\r\n"));
	CHECK(find_send("BYE") < 0);
	fake_advance(CALL_XFER_BYE_GRACE_MS - 1);
	CHECK(find_send("BYE") < 0);
	fake_advance(1);
	CHECK_UINT(count_send("BYE"), 1);
	CHECK(req_of("BYE", &ri));
	CHECK(!strcmp(ri.to_tag, "peer-tag"));
	CHECK_UINT(ev.xfer_done, 1);
	reply(200, NULL, NULL, NULL);
	fake_advance(6000);
	CHECK_UINT(ev.ended, 1);
	CHECK(ev.end.transferred);
	CHECK(slot_is(0));

	/* nobody is left to resume a drained leg, so every failure behind the
	 * 202 ends it, and none of them reports: the 202 consumed the report */
	CASE("drain: a verdict of failure behind the 202 BYEs the drained leg and reports nothing");
	fx_reset();
	uac_up(NULL);
	CHECK(call_xfer(0, 0, CALL_XFER_BLIND, "1003"));
	reply(202, NULL, NULL, NULL);
	CHECK_UINT(ev.xfer_done, 1);
	rx(xfer_notify(2, 2, "terminated;reason=noresource", "SIP/2.0 486 Busy Here\r\n"));
	CHECK_UINT(count_send("BYE"), 1);
	CHECK_UINT(ev.xfer_done, 1);
	CHECK(ev.xfer_ok);

	CASE("drain: a subscription that ends with no sipfrag BYEs it too");
	fx_reset();
	uac_up(NULL);
	CHECK(call_xfer(0, 0, CALL_XFER_BLIND, "1003"));
	reply(202, NULL, NULL, NULL);
	rx(xfer_notify(2, 2, "terminated;reason=noresource", NULL));
	CHECK_UINT(count_send("BYE"), 1);
	CHECK_UINT(ev.xfer_done, 1);

	CASE("drain: the guard expiry BYEs the drained leg");
	fx_reset();
	uac_up(NULL);
	CHECK(call_xfer(0, 0, CALL_XFER_BLIND, "1003"));
	reply(202, NULL, NULL, NULL);
	fake_advance(CALL_XFER_WAIT_MS - 1);
	CHECK(find_send("BYE") < 0);
	fake_advance(1);
	CHECK_UINT(count_send("BYE"), 1);
	CHECK_UINT(ev.xfer_done, 1);

	/* the drained leg gives its slot up to a new call, but a free slot is
	 * taken first, and the drain is BYEd when nothing else is left */
	CASE("drain: a dial on the drained name lands on the free slot and BYEs nothing");
	fx_reset();
	uac_up(NULL);
	CHECK(call_xfer(0, 0, CALL_XFER_BLIND, "1003"));
	reply(202, NULL, NULL, NULL);
	CHECK(dial(0, 0, "1004"));
	CHECK(has(last_text(), "INVITE sip:1004@" T_DOMAIN));
	CHECK(find_send("BYE") < 0);
	CHECK(slot_view_get(0, 1, &sv));
	CHECK(sv.has_ui_slot);
	CHECK_UINT(sv.ui_slot, 0);
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(sv.shadow);
	CHECK(!strcmp(sv.state, "active"));

	CASE("drain: a new call that still needs the drained slot BYEs it on the way in");
	CHECK(dial(0, 1, "1005"));
	CHECK_UINT(count_send("BYE"), 1);
	CHECK(req_of("BYE", &ri));
	CHECK(!strcmp(ri.to_tag, "peer-tag"));
	CHECK(has(last_text(), "INVITE sip:1005@" T_DOMAIN));
	CHECK_UINT(ev.ended, 1);
	CHECK(ev.end.transferred);
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!sv.shadow);
	CHECK(sv.has_ui_slot);
	CHECK_UINT(sv.ui_slot, 1);

	CASE("blind: a 202 for a leg whose transfer nobody asked for changes nothing");
	fx_reset();
	uac_up(NULL);
	CHECK(call_flash_relay(0, 0));
	reply(202, "peer-tag", NULL, NULL);
	CHECK_UINT(ev.xfer_done, 0);
	CHECK(find_send("BYE") < 0);
	CHECK(slot_is(1));

	CASE("blind: an inbound leg transfers on its own dialog orientation");
	fx_reset();
	uas_up("call-x2");
	CHECK(call_xfer(0, 0, CALL_XFER_BLIND, "1003"));
	CHECK(req_of("REFER", &ri));
	CHECK(has(last_text(), "Refer-To: <sip:1003@" T_DOMAIN ">\r\n"));

	/* the dialog's local URI is what the From of every in-dialog request of
	 * this leg carries, so the referrer identity cannot drift from it */
	CASE("blind: and refers under the identity that dialog carries");
	CHECK(has(last_text(), "Referred-By: <sip:1001@" T_DOMAIN ">\r\n"));
	CHECK(has(ri.from, "sip:1001@" T_DOMAIN));

	CASE("blind: a withheld call refers under its anonymous identity");
	fx_reset();
	CHECK(dial_present(0, 0, "1002", false));
	reply(100, NULL, NULL, NULL);
	reply(200, "peer-tag", NULL, sdp_body(T_PEER, 20000, "0 101", NULL));
	CHECK(call_xfer(0, 0, CALL_XFER_BLIND, "1003"));
	CHECK(has(last_text(), "Referred-By: <sip:anonymous@anonymous.invalid>\r\n"));
	CHECK(!has(last_text(), "Referred-By: <sip:1001"));
}

static void test_xfer_attended(void)
{
	struct req_info ri;
	const struct sip_timer *guard;
	struct slot_view sv;
	unsigned int refers, answered;

	/*
	 * The guard takes the timer id of the setup bound whose value it
	 * inherits (call.h: it is timer C), and the two are never armed on one
	 * leg: t_c is cancelled long before a leg is confirmed enough to carry
	 * a REFER, so this id names the guard alone in every case below.
	 */
	CASE("attended: the 202 arms the guard instead of BYEing the leg");
	xfer_up(CALL_XFER_ATTENDED);
	CHECK_UINT(count_send("REFER"), 1);
	CHECK_UINT(ev.xfer_done, 0);
	CHECK(find_send("BYE") < 0);
	CHECK(timer_armed(SIP_TIMER_CALL_C));
	CHECK_UINT(timer_delay(SIP_TIMER_CALL_C), CALL_XFER_WAIT_MS);

	CASE("attended: the call is still up and untouched while it waits");
	CHECK(slot_is(1));
	CHECK_UINT(med.stop_calls, 0);
	CHECK_UINT(ev.failed, 0);

	/*
	 * The guard is named by its object from here. Exactly one timer
	 * carries that id while the transfer waits, so the pointer taken now is
	 * the guard and no later case has to trust the id alone.
	 */
	CASE("attended: exactly one timer carries that id, and it is the guard");
	CHECK_UINT(timer_count(SIP_TIMER_CALL_C), 1);
	guard = timer_obj(SIP_TIMER_CALL_C);
	CHECK(guard != NULL);
	CHECK(timer_obj_armed(guard));

	CASE("attended: the first NOTIFY is progress, not an outcome");
	rx(xfer_notify(2, 2, "active;expires=60", "SIP/2.0 100 Trying\r\n"));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(ev.xfer_done, 0);
	CHECK(timer_armed(SIP_TIMER_CALL_C));
	CHECK(find_send("BYE") < 0);

	CASE("attended: a sipfrag 2xx reports that status and graces the leg, no BYE yet");
	answered = fk.sends;
	rx(xfer_notify(3, 2, "terminated;reason=noresource", "SIP/2.0 200 OK\r\n"));
	CHECK(has(send_text(answered), "SIP/2.0 200"));
	CHECK_UINT(ev.xfer_done, 1);
	CHECK(ev.xfer_ok);
	CHECK_UINT(ev.xfer_code, 200);
	CHECK(find_send("BYE") < 0);
	CHECK_UINT(ev.xfer_byes, 0);

	CASE("attended: and the guard OBJECT is re-armed as the BYE grace");
	CHECK(timer_obj_armed(guard));
	CHECK_UINT((unsigned int)timer_delay_obj(guard), CALL_XFER_BYE_GRACE_MS);
	CHECK_UINT((unsigned int)timer_delay_obj(guard), 1000);
	CHECK_UINT(timer_count(SIP_TIMER_CALL_C), 1);

	/*
	 * The ok frees the UI slot at once (call.h: the FSM owns the slot roles
	 * from that instant). The leg drains for the network's BYE, so it is
	 * still in use, and the dump is what tells the two occupancies apart.
	 */
	CASE("attended: the ok hands the UI slot back while the leg drains");
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.state, "active"));
	CHECK(!sv.ui_busy);
	CHECK(sv.shadow);

	/* the drained leg still owns its dialog, so a dial on its name takes
	 * the free physical slot and leaves it to finish */
	CASE("attended: so the line takes a new call under that name at once, on the free slot");
	CHECK(dial(0, 0, "1004"));
	CHECK(has(last_text(), "INVITE sip:1004@" T_DOMAIN));
	CHECK(find_send("BYE") < 0);
	CHECK(slot_view_get(0, 1, &sv));
	CHECK(sv.has_ui_slot);
	CHECK_UINT(sv.ui_slot, 0);
	CHECK_UINT(ev.ended, 0);

	CASE("attended: with no network BYE the grace ends the drained leg with this side's");
	fake_advance(CALL_XFER_BYE_GRACE_MS);
	CHECK_UINT(count_send("BYE"), 1);
	reply_at((unsigned int)find_send("BYE"), 200, NULL, NULL, NULL);
	fake_advance(6000);

	CASE("attended: the record of the transferred leg says a transfer ended it");
	CHECK_UINT(ev.ended, 1);
	CHECK(ev.end.transferred);
	CHECK_UINT(ev.failed, 0);
	CHECK_UINT(ev.remote_bye, 0);

	CASE("attended: a sipfrag 486 fails the transfer and keeps the call");
	xfer_up(CALL_XFER_ATTENDED);
	rx(xfer_notify(2, 2, "terminated;reason=noresource", "SIP/2.0 486 Busy Here\r\n"));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(ev.xfer_done, 1);
	CHECK(!ev.xfer_ok);
	CHECK_UINT(ev.xfer_code, 486);

	CASE("attended: the leg the transfer failed on is still up");
	CHECK(find_send("BYE") < 0);
	CHECK_UINT(med.stop_calls, 0);
	CHECK_UINT(ev.failed, 0);
	CHECK_UINT(ev.ended, 0);
	CHECK(slot_is(1));

	CASE("attended: and it still takes an ordinary command after the failure");
	CHECK(call_hold(0, 0));
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.method, "INVITE"));

	CASE("attended: a subscription that ends with no verdict fails it too");
	xfer_up(CALL_XFER_ATTENDED);
	rx(xfer_notify(2, 2, "terminated;reason=noresource", NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(ev.xfer_done, 1);
	CHECK(!ev.xfer_ok);
	CHECK_UINT(ev.xfer_code, 408);
	CHECK(find_send("BYE") < 0);
	CHECK(slot_is(1));

	/* the boundary of the verdict rule: 299 is the last ok and 300 the
	 * first failure, so a comparison that slipped one way is caught */
	CASE("attended: a sipfrag 299 is still an ok outcome");
	xfer_up(CALL_XFER_ATTENDED);
	rx(xfer_notify(2, 2, "terminated", "SIP/2.0 299 Nearly\r\n"));
	CHECK_UINT(ev.xfer_done, 1);
	CHECK(ev.xfer_ok);
	CHECK_UINT(ev.xfer_code, 299);
	CHECK(find_send("BYE") < 0);
	CHECK_UINT((unsigned int)timer_delay(SIP_TIMER_CALL_C), CALL_XFER_BYE_GRACE_MS);

	CASE("attended: and a sipfrag 300 is the first failing one");
	xfer_up(CALL_XFER_ATTENDED);
	rx(xfer_notify(2, 2, "terminated", "SIP/2.0 300 Multiple Choices\r\n"));
	CHECK_UINT(ev.xfer_done, 1);
	CHECK(!ev.xfer_ok);
	CHECK_UINT(ev.xfer_code, 300);
	CHECK(find_send("BYE") < 0);
	CHECK(slot_is(1));

	/*
	 * RFC 3515 2.4.5: the verdict rides a message/sipfrag body and nothing
	 * else. A body of another type that merely looks like a status line
	 * says nothing about the transfer.
	 */
	CASE("attended: a status line under another content type is no verdict");
	xfer_up(CALL_XFER_ATTENDED);
	answered = fk.sends;
	rx(uac_in_dialog("NOTIFY", "peer-tag", 2,
			 "Event: refer;id=2\r\n"
			 "Subscription-State: active;expires=60\r\n"
			 "Content-Type: text/plain\r\n",
			 "SIP/2.0 200 OK\r\n"));
	CHECK(has(send_text(answered), "SIP/2.0 200"));
	CHECK_UINT(ev.xfer_done, 0);
	CHECK(timer_armed(SIP_TIMER_CALL_C));
	CHECK(find_send("BYE") < 0);

	CASE("attended: and a body with no content type at all is none either");
	answered = fk.sends;
	rx(uac_in_dialog("NOTIFY", "peer-tag", 3,
			 "Event: refer;id=2\r\n"
			 "Subscription-State: active;expires=60\r\n",
			 "SIP/2.0 200 OK\r\n"));
	CHECK(has(send_text(answered), "SIP/2.0 200"));
	CHECK_UINT(ev.xfer_done, 0);

	/* the sipfrag parameters ride along, and the type is compared without
	 * case, which is the rule the other body of this leg takes */
	CASE("attended: the sipfrag type is matched with its parameters and without case");
	rx(uac_in_dialog("NOTIFY", "peer-tag", 4,
			 "Event: refer;id=2\r\n"
			 "Subscription-State: terminated\r\n"
			 "Content-Type: Message/SipFrag;version=2.0\r\n",
			 "SIP/2.0 200 OK\r\n"));
	CHECK_UINT(ev.xfer_done, 1);
	CHECK(ev.xfer_ok);

	/*
	 * A final NOTIFY can overtake the answer to its own REFER. The
	 * subscription exists from the moment the REFER goes out, and nothing
	 * orders the two on UDP. The verdict waits for that answer.
	 */
	CASE("reorder: a verdict that arrives before the 202 decides nothing yet");
	fx_reset();
	uac_up(NULL);
	CHECK(call_xfer(0, 0, CALL_XFER_ATTENDED, "1003"));
	answered = fk.sends;
	rx(xfer_notify(2, 2, "terminated", "SIP/2.0 200 OK\r\n"));
	CHECK(has(send_text(answered), "SIP/2.0 200"));
	CHECK_UINT(ev.xfer_done, 0);
	CHECK(find_send("BYE") < 0);
	CHECK(slot_is(1));

	CASE("reorder: and the 202 releases it, with the status the NOTIFY named");
	reply_at((unsigned int)find_send("REFER"), 202, NULL, NULL, NULL);
	CHECK_UINT(ev.xfer_done, 1);
	CHECK(ev.xfer_ok);
	CHECK_UINT(ev.xfer_code, 200);
	CHECK(find_send("BYE") < 0);
	CHECK_UINT(ev.xfer_byes, 0);
	CHECK_UINT((unsigned int)timer_delay(SIP_TIMER_CALL_C), CALL_XFER_BYE_GRACE_MS);

	CASE("reorder: a REFER that FAILS behind that verdict wins, and once");
	fx_reset();
	uac_up(NULL);
	CHECK(call_xfer(0, 0, CALL_XFER_ATTENDED, "1003"));
	rx(xfer_notify(2, 2, "terminated", "SIP/2.0 200 OK\r\n"));
	reply_at((unsigned int)find_send("REFER"), 603, NULL, NULL, NULL);
	CHECK_UINT(ev.xfer_done, 1);
	CHECK(!ev.xfer_ok);
	CHECK_UINT(ev.xfer_code, 603);

	CASE("reorder: so the leg the REFER lost is still up and untouched");
	CHECK(find_send("BYE") < 0);
	CHECK_UINT(med.stop_calls, 0);
	CHECK_UINT(ev.failed, 0);
	CHECK(slot_is(1));

	CASE("reorder: and a later transfer on that leg carries no stale verdict");
	CHECK(call_xfer(0, 0, CALL_XFER_ATTENDED, "1004"));
	reply_at((unsigned int)find_send("REFER"), 202, NULL, NULL, NULL);
	CHECK_UINT(ev.xfer_done, 1);
	CHECK(timer_armed(SIP_TIMER_CALL_C));
	CHECK(find_send("BYE") < 0);

	/* the release runs in front of the blind settle: a failure that
	 * overtook the 202 belongs to a leg the subscriber is still on */
	CASE("reorder: a blind transfer takes the overtaking verdict the same way");
	fx_reset();
	uac_up(NULL);
	CHECK(call_xfer(0, 0, CALL_XFER_BLIND, "1003"));
	rx(xfer_notify(2, 2, "terminated", "SIP/2.0 486 Busy Here\r\n"));
	CHECK_UINT(ev.xfer_done, 0);
	reply_at((unsigned int)find_send("REFER"), 202, NULL, NULL, NULL);
	CHECK_UINT(ev.xfer_done, 1);
	CHECK(!ev.xfer_ok);
	CHECK_UINT(ev.xfer_code, 486);
	CHECK(find_send("BYE") < 0);

	CASE("reorder: and that leg is still the subscriber's, untouched and usable");
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(sv.ui_busy);
	CHECK(!sv.shadow);
	CHECK(!strcmp(sv.state, "active"));
	CHECK_UINT(med.stop_calls, 0);
	CHECK(call_hold(0, 0));
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.method, "INVITE"));

	CASE("reorder: a blind 2xx that overtook the 202 graces the leg at the 202");
	fx_reset();
	uac_up(NULL);
	CHECK(call_xfer(0, 0, CALL_XFER_BLIND, "1003"));
	rx(xfer_notify(2, 2, "terminated", "SIP/2.0 200 OK\r\n"));
	CHECK_UINT(ev.xfer_done, 0);
	reply_at((unsigned int)find_send("REFER"), 202, NULL, NULL, NULL);
	CHECK_UINT(ev.xfer_done, 1);
	CHECK(ev.xfer_ok);
	CHECK_UINT(ev.xfer_code, 200);
	CHECK(find_send("BYE") < 0);
	CHECK_UINT((unsigned int)timer_delay(SIP_TIMER_CALL_C), CALL_XFER_BYE_GRACE_MS);

	CASE("attended: a body that is no status line at all is no verdict either");
	xfer_up(CALL_XFER_ATTENDED);
	rx(xfer_notify(2, 2, "active;expires=60", "not a status line\r\n"));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(ev.xfer_done, 0);
	CHECK(timer_armed(SIP_TIMER_CALL_C));

	CASE("attended: the guard expiry is the failure verdict, reported 408");
	fake_advance(CALL_XFER_WAIT_MS);
	CHECK_UINT(ev.xfer_done, 1);
	CHECK(!ev.xfer_ok);
	CHECK_UINT(ev.xfer_code, 408);

	CASE("attended: and the guard leaves the call standing");
	CHECK(find_send("BYE") < 0);
	CHECK_UINT(ev.failed, 0);
	CHECK_UINT(med.stop_calls, 0);
	CHECK(slot_is(1));

	CASE("attended: nothing fires a second time after the expiry");
	fake_advance(CALL_XFER_WAIT_MS);
	CHECK_UINT(ev.xfer_done, 1);

	CASE("attended: a NOTIFY whose id names another subscription is answered 481");
	xfer_up(CALL_XFER_ATTENDED);
	rx(xfer_notify(2, 99, "active;expires=60", "SIP/2.0 200 OK\r\n"));
	CHECK(has(last_text(), "SIP/2.0 481"));
	CHECK_UINT(ev.xfer_done, 0);
	CHECK(find_send("BYE") < 0);

	CASE("attended: a NOTIFY with no id at all names the one outstanding refer");
	answered = fk.sends;
	rx(xfer_notify_text(3, "refer", "terminated;reason=noresource", "SIP/2.0 200 OK\r\n"));
	CHECK(has(send_text(answered), "SIP/2.0 200"));
	CHECK_UINT(ev.xfer_done, 1);
	CHECK(ev.xfer_ok);

	CASE("attended: a refer NOTIFY on a leg with no transfer is answered 481");
	fx_reset();
	uac_up(NULL);
	rx(xfer_notify(2, 2, "active;expires=60", "SIP/2.0 200 OK\r\n"));
	CHECK(has(last_text(), "SIP/2.0 481"));
	CHECK_UINT(ev.xfer_done, 0);
	CHECK(slot_is(1));

	/*
	 * The account intake answers the packages it serves, in the fixture as
	 * in the daemon: a message summary that matches a dialog is consumed
	 * there and never reaches the leg. The routing key itself is
	 * test_sip_sub's and test_account's.
	 */
	CASE("attended: a message summary in dialog is the account layer's, not the leg's");
	rx(xfer_notify_text(3, "message-summary", "active", "Messages-Waiting: yes\r\n"));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(fk.mwi_notifies, 1);
	CHECK_UINT(ev.xfer_done, 0);

	/* the acknowledge-and-drop is still the leg's answer for a package
	 * that reaches it: a 481 there would break a call over an unread body,
	 * so the case drives the leg directly */
	CASE("attended: a package that does reach the leg keeps its 200");
	rx_in_dialog_direct(0, xfer_notify_text(4, "presence", "active", NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(ev.xfer_done, 0);

	/* and a refer NOTIFY travels account.c's request intake, which skips
	 * the NOTIFY intake: it is answered by the leg, not by the account */
	CASE("attended: while a refer NOTIFY reaches the leg through account.c");
	fx_reset();
	uac_up(NULL);
	rx(xfer_notify(2, 2, "active;expires=60", "SIP/2.0 200 OK\r\n"));
	CHECK(has(last_text(), "SIP/2.0 481"));
	CHECK_UINT(fk.mwi_notifies, 0);

	CASE("attended: a non-2xx answer to the REFER fails with that status");
	fx_reset();
	uac_up(NULL);
	CHECK(call_xfer(0, 0, CALL_XFER_ATTENDED, "1003"));
	reply(603, NULL, NULL, NULL);
	CHECK_UINT(ev.xfer_done, 1);
	CHECK(!ev.xfer_ok);
	CHECK_UINT(ev.xfer_code, 603);
	CHECK(!timer_armed(SIP_TIMER_CALL_C));
	CHECK(find_send("BYE") < 0);
	CHECK(slot_is(1));

	CASE("attended: a REFER that dies without an answer fails with 408");
	fx_reset();
	uac_up(NULL);
	CHECK(call_xfer(0, 0, CALL_XFER_ATTENDED, "1003"));
	refers = count_send("REFER");
	fake_advance(40000);
	CHECK_UINT(ev.xfer_done, 1);
	CHECK(!ev.xfer_ok);
	CHECK_UINT(ev.xfer_code, 408);
	CHECK(count_send("REFER") >= refers);
	CHECK(slot_is(1));

	CASE("attended: and the leg takes a new transfer after that failure");
	CHECK(call_xfer(0, 0, CALL_XFER_ATTENDED, "1004"));
	CHECK(has(last_text(), "Refer-To: <sip:1004@" T_DOMAIN ">\r\n"));
	CHECK(has(last_text(), "Contact: <sip:1001@" T_HOST ":5060>\r\n"));
}

/*
 * A quoted-string field of n quote bytes, as it rides the wire: RFC 3261 25.1
 * escapes each one, so the parser keeps 2n bytes and hdr_cred_write() escapes
 * both of those again when it re-emits them. That doubling is the one expansion
 * call.h leaves deliberately unbudgeted (CALL_WIRE_EXTRA_MAX), and it is what
 * makes a peer able to push the header block past its buffer.
 */
static const char *quote_fill(unsigned int n)
{
	char *buf = text_slot();
	unsigned int i;

	for (i = 0; i < n && 2 * i + 2 < TEXT_MAX; i++) {
		buf[2 * i] = 0x5c;
		buf[2 * i + 1] = 0x22;
	}
	buf[2 * i] = 0;

	return buf;
}

/*
 * The refuse-not-truncate rule at the buffer itself. The budget of call.h
 * fits the worst REFER that a well behaved peer can draw. It records one
 * expansion that it does not cover: a realm, a nonce or an opaque made of
 * quote bytes, which the credential writer doubles. Such a peer overflows the
 * block, and the request is refused rather than truncated. This case drives
 * that end to end.
 */
static void test_xfer_overflow(void)
{
	char chal[4096];
	char contact[400];
	char tag[256];
	char user[281];
	unsigned int i;

	fx_reset();
	/*
	 * The widest remote target a request URI can hold, on both legs: it is
	 * the URI every in-dialog request of that leg is built on, so it fills
	 * the Refer-To of the transfer and the uri field of every credential.
	 */
	memset(user, 'u', sizeof(user) - 1);
	user[sizeof(user) - 1] = 0;
	snprintf(contact, sizeof(contact), "Contact: <sip:%s@" T_PEER ":5060>\r\n", user);

	/*
	 * The widest embedded Replaces: a peer tag of bytes that every one of
	 * them escapes to %XX, so the raw value fills its own buffer and the
	 * escape trebles it inside the Refer-To.
	 */
	for (i = 0; i < sizeof(tag) - 1; i++)
		tag[i] = '@';
	tag[188] = 0;

	CHECK(dial(0, 0, "1002"));
	reply(100, NULL, NULL, NULL);
	reply(200, "peer-a", contact, sdp_body(T_PEER, 20000, "0 101", NULL));
	CHECK(dial(0, 1, "1003"));
	reply(100, NULL, NULL, NULL);
	reply(200, tag, contact, sdp_body(T_PEER, 20002, "0 101", NULL));
	CHECK_UINT(ev.answered, 2);

	CASE("overflow: the widest REFER a peer can draw still reaches the wire");
	CHECK(call_xfer_consult(0, 0, 1));
	CHECK_UINT(count_send("REFER"), 1);
	CHECK_UINT(tr.extra_refused_lines, 0);
	CHECK_UINT(tr.wire_refused_lines, 0);
	CHECK(has(last_text(), "Refer-To: <sip:"));
	CHECK(has(last_text(), "%40%40%40"));

	/*
	 * Both challenge kinds in one answer, which is the shape the
	 * budget names, and every quoted field of both at the width its own
	 * store allows. The parser unescapes what it keeps and the credential
	 * writer escapes it again, so a field of quote bytes crosses at twice
	 * its stored width: that is the one expansion call.h leaves
	 * deliberately unbudgeted, and it is what pushes this block past it.
	 */
	snprintf(chal, sizeof(chal),
		 "WWW-Authenticate: Digest realm=\"%s\", nonce=\"%s\", opaque=\"%s\", "
		 "qop=\"auth\", algorithm=MD5\r\n"
		 "Proxy-Authenticate: Digest realm=\"%s\", nonce=\"%s\", opaque=\"%s\", "
		 "qop=\"auth\", algorithm=MD5\r\n",
		 quote_fill(127), quote_fill(255), quote_fill(127),
		 quote_fill(127), quote_fill(255), quote_fill(127));

	CASE("overflow: a peer whose credentials double past the buffer is refused");
	reply_at((unsigned int)find_send("REFER"), 401, NULL, chal, NULL);
	CHECK_UINT(count_send("REFER"), 1);
	CHECK_UINT(tr.extra_refused_lines, 1);

	CASE("overflow: nothing of it was truncated onto the wire");
	CHECK(!has(last_text(), "Authorization"));
	CHECK(!has(last_text(), "Refer-To"));

	CASE("overflow: and the transfer fails on the status that challenged it, unreported");
	CHECK_UINT(ev.xfer_done, 0);

	/* the consult form is fire and forget, so the failure clears both legs
	 * exactly as any other failure of it does */
	CASE("overflow: the on-hook form still clears both legs on that failure");
	CHECK_UINT(count_send("BYE"), 2);
	CHECK_UINT(ev.failed, 0);

	/*
	 * The second refusal of the same insert. A shorter peer tag keeps the
	 * header block inside its own buffer and the request still passes
	 * CALL_WIRE_MAX, which the block alone cannot prove: the fixed headers
	 * and the body ride the wire buffer beside it.
	 */
	CASE("overflow: a block that fits its buffer but not the wire is refused "
	     "there");
	tag[4] = 0;
	fx_reset();
	CHECK(dial(0, 0, "1002"));
	reply(100, NULL, NULL, NULL);
	reply(200, "peer-a", contact, sdp_body(T_PEER, 20000, "0 101", NULL));
	CHECK(dial(0, 1, "1003"));
	reply(100, NULL, NULL, NULL);
	reply(200, tag, contact, sdp_body(T_PEER, 20002, "0 101", NULL));
	CHECK(call_xfer_consult(0, 0, 1));
	reply_at((unsigned int)find_send("REFER"), 401, NULL, chal, NULL);
	CHECK_UINT(count_send("REFER"), 1);
	CHECK_UINT(tr.wire_refused_lines, 1);
	CHECK_UINT(tr.extra_refused_lines, 0);

	CASE("overflow: and that refusal clears both legs as the other one does");
	CHECK_UINT(count_send("BYE"), 2);
	CHECK_UINT(ev.xfer_done, 0);
	CHECK_UINT(ev.failed, 0);
}

static void test_xfer_auth(void)
{
	const char *chal = "WWW-Authenticate: Digest realm=\"" T_DOMAIN "\", nonce=\"" T_NONCE
			   "\"\r\n";
	const char *pchal = "Proxy-Authenticate: Digest realm=\"" T_DOMAIN "\", nonce=\"" T_NONCE
			    "\"\r\n";
	const char *pchal2 = "Proxy-Authenticate: Digest realm=\"" T_DOMAIN
			     "\", nonce=\"a-second-nonce\"\r\n";
	struct req_info first, retry;
	unsigned int answered;

	fx_reset();
	uac_up(NULL);
	CHECK(call_xfer(0, 0, CALL_XFER_ATTENDED, "1003"));
	CHECK(req_of("REFER", &first));

	CASE("refer auth: a 401 is answered with one retry");
	reply(401, NULL, chal, NULL);
	CHECK_UINT(count_send("REFER"), 2);
	CHECK_UINT(ev.xfer_done, 0);

	CASE("refer auth: the retried REFER carries the credentials");
	CHECK(has(last_text(), "Authorization: Digest"));
	CHECK(has(last_text(), "nonce=\"" T_NONCE "\""));

	CASE("refer auth: and still carries the whole referral");
	CHECK(has(last_text(), "Refer-To: <sip:1003@" T_DOMAIN ">\r\n"));
	CHECK(has(last_text(), "Referred-By: <sip:1001@" T_DOMAIN ">\r\n"));
	CHECK(has(last_text(), "Contact: <sip:1001@" T_HOST ":5060>\r\n"));

	CASE("refer auth: the retry keeps the dialog and moves the CSeq");
	CHECK(req_of("REFER", &retry));
	CHECK(!strcmp(retry.call_id, first.call_id));
	CHECK(!strcmp(retry.from_tag, first.from_tag));
	CHECK_UINT(retry.cseq, first.cseq + 1);

	CASE("refer auth: the accepted retry arms the guard on the NEW subscription");
	reply(202, NULL, NULL, NULL);
	CHECK(timer_armed(SIP_TIMER_CALL_C));
	rx(xfer_notify(4, first.cseq, "terminated", "SIP/2.0 200 OK\r\n"));
	CHECK(has(last_text(), "SIP/2.0 481"));
	CHECK_UINT(ev.xfer_done, 0);

	CASE("refer auth: and the NOTIFY of the retried REFER carries the outcome");
	answered = fk.sends;
	rx(xfer_notify(5, retry.cseq, "terminated", "SIP/2.0 200 OK\r\n"));
	CHECK(has(send_text(answered), "SIP/2.0 200"));
	CHECK_UINT(ev.xfer_done, 1);
	CHECK(ev.xfer_ok);

	CASE("refer auth: a proxy challenge is its own exchange");
	fx_reset();
	uac_up(NULL);
	CHECK(call_xfer(0, 0, CALL_XFER_BLIND, "1003"));
	reply(407, NULL, pchal, NULL);
	CHECK_UINT(count_send("REFER"), 2);
	CHECK(has(last_text(), "Proxy-Authorization: Digest"));

	CASE("refer auth: a second challenge of the same kind gives up");
	reply(407, NULL, pchal2, NULL);
	CHECK_UINT(count_send("REFER"), 2);
	CHECK_UINT(ev.xfer_done, 1);
	CHECK(!ev.xfer_ok);
	CHECK_UINT(ev.xfer_code, 407);

	CASE("refer auth: and the leg the challenge refused is still up");
	CHECK(find_send("BYE") < 0);
	CHECK_UINT(ev.failed, 0);
	CHECK(slot_is(1));

	CASE("refer auth: a challenge we cannot answer fails the transfer");
	fx_reset();
	uac_up(NULL);
	CHECK(call_xfer(0, 0, CALL_XFER_BLIND, "1003"));
	reply(401, NULL, "WWW-Authenticate: Basic realm=\"x\"\r\n", NULL);
	CHECK_UINT(count_send("REFER"), 1);
	CHECK_UINT(ev.xfer_done, 1);
	CHECK(!ev.xfer_ok);
	CHECK_UINT(ev.xfer_code, 401);
	CHECK(slot_is(1));
}

/*
 * The voiding rule (call.h "A leg that dies first voids the pending outcome").
 * The transferee can BYE the leg as soon as it accepts the referral, which is
 * before the NOTIFY. The leg then ends through the ordinary far-end path, and
 * no xfer_done follows at all.
 */
static void test_xfer_void(void)
{
	CASE("void: a far-end BYE during the wait ends the leg the ordinary way");
	xfer_up(CALL_XFER_ATTENDED);
	rx(uac_in_dialog("BYE", "peer-tag", 2, NULL, NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(ev.remote_bye, 1);
	CHECK_UINT(ev.ended, 1);

	CASE("void: and no transfer outcome follows it");
	CHECK_UINT(ev.xfer_done, 0);
	CHECK(!timer_armed(SIP_TIMER_CALL_C));
	CHECK(slot_is(0));

	CASE("void: a NOTIFY that arrives after that death is answered 481");
	rx(xfer_notify(3, 2, "terminated", "SIP/2.0 200 OK\r\n"));
	CHECK(has(last_text(), "SIP/2.0 481"));
	CHECK_UINT(ev.xfer_done, 0);

	/* the session has to die before the guard, which is timer C at 180 s,
	 * so the peer names an interval whose watchdog fires inside that */
	CASE("void: a session watchdog death during the wait voids it too");
	fx_reset();
	uac_up("Session-Expires: 120;refresher=uas\r\n");
	CHECK(call_xfer(0, 0, CALL_XFER_ATTENDED, "1003"));
	reply(202, NULL, NULL, NULL);
	CHECK(timer_armed(SIP_TIMER_CALL_WATCHDOG));
	fake_advance(88000);
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.cls, CALL_FAIL_QUIET);
	CHECK_UINT(ev.xfer_done, 0);

	CASE("void: a local hangup during the wait voids it as well");
	xfer_up(CALL_XFER_ATTENDED);
	CHECK(call_hangup(0, 0));
	CHECK(find_send("BYE") > 0);
	CHECK_UINT(ev.xfer_done, 0);
	CHECK(!timer_armed(SIP_TIMER_CALL_C));

	CASE("void: and the guard cannot fire on the leg it released");
	reply(200, NULL, NULL, NULL);
	fake_advance(CALL_XFER_WAIT_MS * 2);
	CHECK_UINT(ev.xfer_done, 0);
	CHECK(slot_is(0));

	CASE("void: a line teardown during the wait reports no outcome either");
	xfer_up(CALL_XFER_ATTENDED);
	calls_line_teardown(0);
	CHECK_UINT(ev.xfer_done, 0);
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.cls, CALL_FAIL_QUIET);

	CASE("void: the record of a voided transfer carries no transfer flag");
	reply(200, NULL, NULL, NULL);
	fake_advance(6000);
	CHECK_UINT(ev.ended, 1);
	CHECK(!ev.end.transferred);
}

/*
 * The on-hook consult transfer (call.h call_xfer_consult). The REFER rides the
 * held leg and hands it the active one through an embedded Replaces; the
 * subscriber is already on hook. The 202 silences both legs and BYEs neither:
 * the referee waits for the verdict NOTIFY under CALL_XFER_CONSULT_WAIT_MS and
 * is BYEd on a sipfrag 2xx, the replaced leg drains for the network's Replaces
 * BYE (RFC 5589); a failure BYEs both. Nothing is reported to the machine.
 */

/* the held referee on slot 0 and the active leg it will hand over on slot 1 */
static void consult_up(void)
{
	fx_reset();
	CHECK(dial(0, 0, "1002"));
	reply(100, NULL, NULL, NULL);
	reply(200, "peer-a", NULL, sdp_body(T_PEER, 20000, "0 101", NULL));
	CHECK(call_hold(0, 0));
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "0 101", "a=recvonly\r\n"));
	CHECK(dial(0, 1, "1003"));
	reply(100, NULL, NULL, NULL);
	reply(200, "peer-b", "Contact: <sip:1003@203.0.113.9:5062>\r\n",
	      sdp_body(T_PEER, 20002, "0 101", NULL));
}

/* the INVITE of the replaced leg, which names its dialog */
static bool consult_replaced(struct req_info *ri)
{
	int idx = find_send("INVITE");

	return idx >= 0 && req_info_at((unsigned int)idx, ri);
}

/* a request from the peer of the held leg, the referee dialog: the first
 * INVITE consult_up() sent, where uac_in_dialog() reads the last one */
static const char *referee_in_dialog(const char *method, unsigned int cseq, const char *extra,
				     const char *body)
{
	struct req_info ri;

	if (!req_info_at(0, &ri) || strcmp(ri.method, "INVITE"))
		return "";

	return dialog_req_text(method, cseq, ri.call_id, "peer-a", ri.from_tag, extra, body);
}

/* the NOTIFY of the referee's implicit subscription, from the peer of the
 * held leg, with the id our REFER handed out; a NULL frag carries no body */
static const char *consult_notify(unsigned int cseq, const char *sub_state, const char *frag)
{
	struct req_info ri;
	char extra[256];

	if (!req_of("REFER", &ri))
		return "";

	snprintf(extra, sizeof(extra),
		 "Event: refer;id=%u\r\n"
		 "Subscription-State: %s\r\n"
		 "Content-Type: message/sipfrag;version=2.0\r\n",
		 ri.cseq, sub_state);

	return referee_in_dialog("NOTIFY", cseq, extra, frag);
}

/* the common start: both legs up, the REFER out and accepted */
static void consult_accepted(void)
{
	consult_up();
	CHECK(call_xfer_consult(0, 0, 1));
	reply(202, NULL, NULL, NULL);
}

/*
 * The credential retry of a consult REFER (RFC 3261 22, the rule that the
 * blind cases of test_xfer_auth() drive). This kind uses two dialogs. The
 * retry builds the embedded Replaces of the replaced one again. A challenge
 * that it cannot answer BYEs both legs and reports nothing.
 */
static void test_xfer_auth_consult(void)
{
	const char *chal = "WWW-Authenticate: Digest realm=\"" T_DOMAIN
			   "\", nonce=\"" T_NONCE "\"\r\n";
	const char *chal2 = "WWW-Authenticate: Digest realm=\"" T_DOMAIN
			    "\", nonce=\"a-second-nonce\"\r\n";
	char realm[SIP_AUTH_REALM_MAX + 1];
	struct req_info replaced;
	char chal_wide[512];
	char want[TEXT_MAX];
	char raw[512];

	consult_up();
	CHECK(consult_replaced(&replaced));
	CHECK(call_xfer_consult(0, 0, 1));

	CASE("refer auth: a consult REFER challenged 401 is retried with credentials");
	reply(401, NULL, chal, NULL);
	CHECK_UINT(count_send("REFER"), 2);
	CHECK(has(last_text(), "Authorization: Digest"));
	CHECK(has(last_text(), "nonce=\"" T_NONCE "\""));
	CHECK(find_send("BYE") < 0);
	CHECK_UINT(med.stop_calls, 0);

	/*
	 * The retry is built again and not replayed, so the escaped Replaces is
	 * built a second time. A retry that lost it would refer the target to a
	 * plain call, which is a blind transfer where the subscriber asked to
	 * join two calls. The suite computes the escape itself.
	 */
	CASE("refer auth: and the retry keeps the embedded Replaces of the other "
	     "dialog");
	snprintf(raw, sizeof(raw), "%s;to-tag=peer-b;from-tag=%s", replaced.call_id,
		 replaced.from_tag);
	snprintf(want, sizeof(want),
		 "Refer-To: <sip:1003@203.0.113.9:5062?Replaces=%s>\r\n", xfer_escape(raw));
	CHECK(has(last_text(), want));

	CASE("refer auth: the retried consult is accepted and silences both legs");
	reply(202, NULL, NULL, NULL);
	CHECK_UINT(med.stop_calls, 2);
	CHECK(find_send("BYE") < 0);

	CASE("refer auth: a second challenge of the same kind BYEs both consult legs");
	consult_up();
	CHECK(call_xfer_consult(0, 0, 1));
	reply(401, NULL, chal, NULL);
	CHECK_UINT(count_send("REFER"), 2);
	reply(401, NULL, chal2, NULL);
	CHECK_UINT(count_send("REFER"), 2);
	CHECK_UINT(count_send("BYE"), 2);
	CHECK_UINT(ev.xfer_done, 0);

	/*
	 * A realm one byte wider than the credential store. The store refuses it
	 * instead of keeping a prefix (proto/sip/auth.c chal_check), because a response
	 * built on half a realm is one no server can check. No credential is
	 * built, so the retry never reaches the header block of the in-dialog
	 * send and neither buffer of that send reports a refusal.
	 */
	CASE("refer auth: a realm too wide for the credential store refuses the "
	     "consult retry");
	memset(realm, 'r', sizeof(realm) - 1);
	realm[sizeof(realm) - 1] = 0;
	snprintf(chal_wide, sizeof(chal_wide),
		 "WWW-Authenticate: Digest realm=\"%s\", nonce=\"" T_NONCE "\"\r\n", realm);
	consult_up();
	CHECK(call_xfer_consult(0, 0, 1));
	reply(401, NULL, chal_wide, NULL);
	CHECK_UINT(count_send("REFER"), 1);
	CHECK_UINT(tr.extra_refused_lines, 0);
	CHECK_UINT(tr.wire_refused_lines, 0);
	CHECK_UINT(count_send("BYE"), 2);
	CHECK_UINT(ev.xfer_done, 0);
}

static void test_xfer_consult(void)
{
	struct req_info ri, replaced;
	struct slot_view sv;
	unsigned int refers, byes;
	char want[TEXT_MAX];
	char raw[512];

	consult_up();
	CHECK(consult_replaced(&replaced));

	CASE("consult: the REFER goes out on the HELD leg");
	CHECK(call_xfer_consult(0, 0, 1));
	CHECK_UINT(count_send("REFER"), 1);
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.method, "REFER"));
	CHECK(!strcmp(ri.to_tag, "peer-a"));

	CASE("consult: the Refer-To is the remote target of the replaced leg");
	CHECK(has(last_text(), "Refer-To: <sip:1003@203.0.113.9:5062?Replaces="));

	/*
	 * The tags are oriented at the receiver (RFC 3891 3): the target
	 * compares to-tag against its own local tag, so our remote tag is the
	 * one that rides to-tag. A swap here transfers the call to a dialog
	 * nobody holds, and every byte of the escape is the suite's own.
	 */
	CASE("consult: with the escaped Replaces of that dialog, receiver oriented");
	snprintf(raw, sizeof(raw), "%s;to-tag=peer-b;from-tag=%s", replaced.call_id,
		 replaced.from_tag);
	snprintf(want, sizeof(want), "Refer-To: <sip:1003@203.0.113.9:5062?Replaces=%s>\r\n",
		 xfer_escape(raw));
	CHECK(has(last_text(), want));

	CASE("consult: Referred-By still carries our own identity");
	CHECK(has(last_text(), "Referred-By: <sip:1001@" T_DOMAIN ">\r\n"));

	CASE("consult: and the held leg's REFER carries our Contact");
	CHECK(has(last_text(), "Contact: <sip:1001@" T_HOST ":5060>\r\n"));

	CASE("consult: nothing is cleared before the answer");
	CHECK(find_send("BYE") < 0);
	CHECK_UINT(ev.xfer_done, 0);
	CHECK_UINT(med.stop_calls, 0);

	/*
	 * The 202 BYEs nothing (RFC 5589 7.1): the
	 * referee waits for the verdict NOTIFY, and the replaced leg drains for
	 * the network's Replaces-driven BYE. A BYE of our own on the replaced
	 * leg at the 202 would destroy the dialog that the network BYE needs,
	 * and answer that BYE 481. A BYE on the referee at the 202 would meet
	 * the verdict NOTIFY with a gone subscription and a 481.
	 */
	CASE("consult: the 202 BYEs neither leg");
	reply(202, NULL, NULL, NULL);
	CHECK(find_send("BYE") < 0);

	CASE("consult: both media streams stopped at the 202");
	CHECK_UINT(med.stop_calls, 2);

	CASE("consult: and no outcome is reported, the machine is idle by contract");
	CHECK_UINT(ev.xfer_done, 0);

	/* call.h: both keep their ui_busy until the protocol ends, so
	 * admission answers a new INVITE 486 while the two legs wait */
	CASE("consult: both legs keep the line occupied while they wait");
	{
		unsigned int slot = 99;

		CHECK(!call_slot_free(0, &slot));
	}

	CASE("consult: the FSM hears no clearing class from either leg");
	CHECK_UINT(ev.failed, 0);
	CHECK_UINT(ev.remote_bye, 0);

	CASE("consult: both legs are live shadows that occupy their slots");
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.state, "active"));
	CHECK(sv.ui_busy);
	CHECK(sv.shadow);
	CHECK(slot_view_get(0, 1, &sv));
	CHECK(!strcmp(sv.state, "active"));
	CHECK(sv.ui_busy);
	CHECK(sv.shadow);

	/* two guards ride the timer id of the setup bound (call.h): the drain
	 * guard of the replaced leg, armed first, and the verdict guard of the
	 * referee */
	CASE("consult: the replaced leg arms the drain guard, the referee the verdict guard");
	CHECK_UINT(timer_count(SIP_TIMER_CALL_C), 2);
	CHECK_UINT((unsigned int)timer_delay_obj(timer_obj_nth(SIP_TIMER_CALL_C, 0)),
		   CALL_XFER_DRAIN_MS);
	CHECK_UINT((unsigned int)timer_delay_obj(timer_obj_nth(SIP_TIMER_CALL_C, 1)),
		   CALL_XFER_CONSULT_WAIT_MS);

	CASE("consult: the verdict NOTIFY is answered 200, not 481");
	rx(consult_notify(2, "terminated;reason=noresource", "SIP/2.0 200 OK\r\n"));
	CHECK(find_send("SIP/2.0 481") < 0);
	CHECK(has(send_text((unsigned int)find_send("SIP/2.0 200")), "CSeq: 2 NOTIFY"));

	CASE("consult: and the sipfrag 2xx graces the referee leg alone, no BYE yet");
	CHECK(find_send("BYE") < 0);
	CHECK_UINT(ev.xfer_done, 0);

	/* both objects are pinned by delay and not by ordinal: the fake timer
	 * table swaps its last entry into a dropped slot */
	CASE("consult: the verdict guard became the BYE grace and the drain guard stands");
	CHECK_UINT(timer_count(SIP_TIMER_CALL_C), 2);
	{
		uint64_t da = timer_delay_obj(timer_obj_nth(SIP_TIMER_CALL_C, 0));
		uint64_t db = timer_delay_obj(timer_obj_nth(SIP_TIMER_CALL_C, 1));

		CHECK((da == CALL_XFER_DRAIN_MS && db == CALL_XFER_BYE_GRACE_MS) ||
		      (db == CALL_XFER_DRAIN_MS && da == CALL_XFER_BYE_GRACE_MS));
		CHECK(da == 1000 || db == 1000);
	}

	CASE("consult: a refer NOTIFY behind the verdict meets no subscription, 481");
	rx(consult_notify(3, "terminated;reason=noresource", "SIP/2.0 200 OK\r\n"));
	CHECK(has(last_text(), "SIP/2.0 481"));
	CHECK(find_send("BYE") < 0);

	CASE("consult: with no network BYE the grace ends the referee with this side's");
	fake_advance(CALL_XFER_BYE_GRACE_MS - 1);
	CHECK(find_send("BYE") < 0);
	fake_advance(1);
	CHECK_UINT(count_send("BYE"), 1);
	CHECK(req_of("BYE", &ri));
	CHECK(!strcmp(ri.to_tag, "peer-a"));
	CHECK_UINT(timer_count(SIP_TIMER_CALL_C), 1);
	CHECK_UINT((unsigned int)timer_delay(SIP_TIMER_CALL_C),
		   CALL_XFER_DRAIN_MS - CALL_XFER_BYE_GRACE_MS);

	/* the referee BYE completes at timer K and closes its record
	 * transferred; the drain guard is three minutes away, so the advance
	 * fires nothing else */
	CASE("consult: answering the referee BYE closes its transferred record");
	reply_at((unsigned int)find_send("BYE"), 200, NULL, NULL, NULL);
	fake_advance(6000);
	CHECK_UINT(ev.ends[0][0].count, 1);
	CHECK(ev.ends[0][0].transferred);

	/* the referee slot is free again for a new call once its BYE
	 * transaction ended, while the replaced leg alone holds the line */
	CASE("consult: the referee slot frees at timer K while the replaced leg drains");
	CHECK(slot_is(0));

	/*
	 * The network's Replaces-driven BYE arrives on the replaced dialog. The
	 * drain leg answers it 200 WITHOUT having sent a BYE of its own first,
	 * tells the FSM nothing, and closes its record transferred.
	 */
	CASE("consult: the drain leg answers the network Replaces BYE 200");
	rx(uac_in_dialog("BYE", "peer-b", 2, NULL, NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(count_send("BYE"), 1);
	CHECK_UINT(ev.remote_bye, 0);
	CHECK_UINT(ev.ends[0][1].count, 1);
	CHECK(ev.ends[0][1].transferred);
	CHECK(slot_is(0));

	/*
	 * The verdict overtook the 202. It is held until the REFER is answered,
	 * and the accept settles both legs before it releases the verdict, so
	 * the replaced leg drains exactly as it does in the ordinary order.
	 */
	CASE("consult: a verdict that overtook the 202 waits for it");
	consult_up();
	CHECK(call_xfer_consult(0, 0, 1));
	rx(consult_notify(2, "terminated;reason=noresource", "SIP/2.0 200 OK\r\n"));
	CHECK(find_send("BYE") < 0);
	CHECK_UINT(med.stop_calls, 0);

	CASE("consult: and the 202 drains the replaced leg and graces the referee at once");
	reply_at((unsigned int)find_send("REFER"), 202, NULL, NULL, NULL);
	CHECK(find_send("BYE") < 0);
	CHECK_UINT(med.stop_calls, 2);
	CHECK_UINT(timer_count(SIP_TIMER_CALL_C), 2);
	fake_advance(CALL_XFER_BYE_GRACE_MS);
	CHECK_UINT(count_send("BYE"), 1);
	CHECK(req_of("BYE", &ri));
	CHECK(!strcmp(ri.to_tag, "peer-a"));
	CHECK_UINT(timer_count(SIP_TIMER_CALL_C), 1);
	CHECK_UINT((unsigned int)timer_delay(SIP_TIMER_CALL_C),
		   CALL_XFER_DRAIN_MS - CALL_XFER_BYE_GRACE_MS);

	CASE("consult: a verdict that overtook a REFUSED REFER is discarded with it");
	consult_up();
	CHECK(call_xfer_consult(0, 0, 1));
	rx(consult_notify(2, "terminated;reason=noresource", "SIP/2.0 200 OK\r\n"));
	reply_at((unsigned int)find_send("REFER"), 603, NULL, NULL, NULL);
	CHECK_UINT(count_send("BYE"), 2);
	CHECK_UINT(timer_count(SIP_TIMER_CALL_C), 0);
	CHECK_UINT(ev.xfer_done, 0);

	/*
	 * A verdict of failure BYEs both legs: nothing replaces either, and the
	 * subscriber is on hook, so nobody is left to resume them.
	 */
	CASE("consult: a sipfrag 486 BYEs both legs");
	consult_accepted();
	rx(consult_notify(2, "terminated;reason=noresource", "SIP/2.0 486 Busy Here\r\n"));
	CHECK_UINT(count_send("BYE"), 2);
	CHECK_UINT(ev.xfer_done, 0);
	CHECK_UINT(timer_count(SIP_TIMER_CALL_C), 0);

	CASE("consult: and a failed transfer marks neither record as transferred");
	reply(200, NULL, NULL, NULL);
	reply_at((unsigned int)find_send("BYE") - 1, 200, NULL, NULL, NULL);
	fake_advance(6000);
	CHECK_UINT(ev.ends[0][0].count, 1);
	CHECK_UINT(ev.ends[0][1].count, 1);
	CHECK(!ev.ends[0][0].transferred);
	CHECK(!ev.ends[0][1].transferred);

	CASE("consult: a subscription that ends with no verdict BYEs both legs too");
	consult_accepted();
	rx(consult_notify(2, "terminated;reason=noresource", NULL));
	CHECK_UINT(count_send("BYE"), 2);
	CHECK_UINT(ev.xfer_done, 0);

	/*
	 * A subscription the switch closes with reason=timeout beside a sipfrag
	 * 2xx is a success (a Metaswitch sends exactly this): the
	 * sipfrag decides before the subscription state is read.
	 */
	CASE("consult: terminated;reason=timeout beside a sipfrag 2xx is a success");
	consult_accepted();
	rx(consult_notify(2, "terminated;reason=timeout", "SIP/2.0 200 OK\r\n"));
	CHECK(find_send("BYE") < 0);
	CHECK_UINT(timer_count(SIP_TIMER_CALL_C), 2);
	fake_advance(CALL_XFER_BYE_GRACE_MS);
	CHECK_UINT(count_send("BYE"), 1);
	CHECK(req_of("BYE", &ri));
	CHECK(!strcmp(ri.to_tag, "peer-a"));
	CHECK_UINT(timer_count(SIP_TIMER_CALL_C), 1);

	/*
	 * The verdict guard. A peer that never notifies leaves the referee
	 * with no verdict: it is BYEd at the deadline, and the replaced leg
	 * keeps draining under its own guard, because a missing NOTIFY is no
	 * verdict and a self-BYE there re-opens the race the drain closed.
	 */
	CASE("consult: the verdict guard BYEs the referee and leaves the replaced leg draining");
	consult_accepted();
	fake_advance(CALL_XFER_CONSULT_WAIT_MS - 1);
	CHECK(find_send("BYE") < 0);
	fake_advance(1);
	CHECK_UINT(count_send("BYE"), 1);
	CHECK(req_of("BYE", &ri));
	CHECK(!strcmp(ri.to_tag, "peer-a"));
	CHECK_UINT(ev.xfer_done, 0);
	CHECK_UINT(timer_count(SIP_TIMER_CALL_C), 1);
	CHECK_UINT((unsigned int)timer_delay(SIP_TIMER_CALL_C),
		   CALL_XFER_DRAIN_MS - CALL_XFER_CONSULT_WAIT_MS);

	CASE("consult: and that drain leg still answers the network BYE 200");
	rx(uac_in_dialog("BYE", "peer-b", 2, NULL, NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(count_send("BYE"), 1);

	/*
	 * The leak fallback. A network that never sends the Replaces BYE leaves
	 * the drain leg alive; the guard BYEs it, but only after
	 * CALL_XFER_DRAIN_MS, because a short guard would BYE first and re-open
	 * the 481 race against a late network BYE.
	 */
	CASE("consult: the drain guard BYEs the replaced leg only after its window");
	consult_accepted();
	rx(consult_notify(2, "terminated;reason=noresource", "SIP/2.0 200 OK\r\n"));
	fake_advance(CALL_XFER_BYE_GRACE_MS);	/* the referee BYE leaves */
	reply(200, NULL, NULL, NULL);		/* and completes */
	CHECK_UINT(count_send("BYE"), 1);
	/* the drain guard was armed at the 202, so the grace already spent
	 * part of its window */
	fake_advance(CALL_XFER_DRAIN_MS - CALL_XFER_BYE_GRACE_MS - 1000);
	CHECK_UINT(count_send("BYE"), 1);
	/* to the deadline exactly: a step past it would count the UDP
	 * retransmission of the unanswered BYE as a third one */
	fake_advance(1000);
	CHECK_UINT(count_send("BYE"), 2);

	CASE("consult: and that fallback BYE rode the replaced dialog");
	CHECK(req_of("BYE", &ri));
	CHECK(!strcmp(ri.to_tag, "peer-b"));

	/*
	 * The fallback BYE is an in-dialog request like any other, so a proxy
	 * that guards the dialog may challenge it, and an unanswered challenge
	 * would leak the call on the far end. It is retried once with
	 * credentials, the rule the BYE path already follows.
	 */
	CASE("consult: a challenged fallback BYE is retried with credentials");
	{
		unsigned int byes = count_send("BYE");

		reply(401, NULL,
		      "WWW-Authenticate: Digest realm=\"" T_DOMAIN "\", nonce=\"" T_NONCE "\"\r\n",
		      NULL);
		CHECK_UINT(count_send("BYE"), byes + 1);
		CHECK(has(last_text(), "Authorization: Digest"));
	}

	/*
	 * A switch that ends both transferor legs itself (a Metaswitch does)
	 * BYEs the referee during the wait. It is answered 200 and
	 * reported to nobody, the outcome is voided with the leg, and the
	 * replaced leg keeps draining under its own guard.
	 */
	CASE("consult: a peer BYE on the referee during the wait is answered 200, reported to nobody");
	consult_accepted();
	rx(referee_in_dialog("BYE",2, NULL, NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(find_send("BYE") < 0);
	CHECK_UINT(ev.remote_bye, 0);
	CHECK_UINT(ev.xfer_done, 0);
	CHECK_UINT(ev.ends[0][0].count, 1);
	CHECK(slot_is(0));

	CASE("consult: and the voided wait leaves the drain guard alone");
	CHECK_UINT(timer_count(SIP_TIMER_CALL_C), 1);
	CHECK_UINT((unsigned int)timer_delay(SIP_TIMER_CALL_C), CALL_XFER_DRAIN_MS);
	fake_advance(CALL_XFER_CONSULT_WAIT_MS + 1000);
	CHECK(find_send("BYE") < 0);

	/*
	 * A peer re-INVITE on the draining replaced leg is answered 200 with a
	 * sendonly answer and no stream: Asterisk re-INVITEs that leg at the
	 * 202 and sends its Replaces BYE only behind a 2xx. A 488 would leave
	 * this leg without that BYE. The answer carries no session timer
	 * either.
	 */
	CASE("consult: a sendrecv re-INVITE on the drain leg is answered 200 sendonly, no stream");
	consult_accepted();
	rx(consult_notify(2, "terminated;reason=noresource", "SIP/2.0 200 OK\r\n"));
	fake_advance(CALL_XFER_BYE_GRACE_MS);
	reply(200, NULL, NULL, NULL);
	fake_advance(6000);
	{
		unsigned int starts = med.start_calls, configs = med.config_calls;

		rx(uac_in_dialog("INVITE", "peer-b", 2, NULL,
				 sdp_body(T_PEER, 20002, "0 101", "a=sendrecv\r\n")));
		CHECK(has(last_text(), "SIP/2.0 200"));
		CHECK(has(last_text(), "a=sendonly"));
		CHECK(!has(last_text(), "Session-Expires"));
		CHECK_UINT(med.start_calls, starts);
		CHECK_UINT(med.config_calls, configs);
		CHECK(!timer_armed(SIP_TIMER_CALL_SESSION));
		CHECK(!timer_armed(SIP_TIMER_CALL_WATCHDOG));
		CHECK_UINT(count_send("BYE"), 1);
		rx(uac_in_dialog("ACK", "peer-b", 2, NULL, NULL));
		CHECK_UINT(med.start_calls, starts);
		CHECK_UINT(med.config_calls, configs);
	}

	CASE("consult: a recvonly re-INVITE on the drain leg is answered sendonly as well");
	rx(uac_in_dialog("INVITE", "peer-b", 3, NULL,
			 sdp_body(T_PEER, 20002, "0 101", "a=recvonly\r\n")));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(has(last_text(), "a=sendonly"));
	rx(uac_in_dialog("ACK", "peer-b", 3, NULL, NULL));

	CASE("consult: and the leg still answers the network BYE 200 behind those answers");
	rx(uac_in_dialog("BYE", "peer-b", 4, NULL, NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(count_send("BYE"), 1);
	CHECK(slot_is(0));
	CHECK(ev.ends[0][1].transferred);

	CASE("consult: a re-INVITE on the referee during the wait takes the same answer");
	consult_accepted();
	rx(referee_in_dialog("INVITE",2, NULL,
			 sdp_body(T_PEER, 20000, "0 101", "a=sendrecv\r\n")));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(has(last_text(), "a=sendonly"));
	rx(referee_in_dialog("ACK",2, NULL, NULL));
	CHECK(find_send("BYE") < 0);
	rx(consult_notify(3, "terminated;reason=noresource", "SIP/2.0 200 OK\r\n"));
	CHECK(find_send("BYE") < 0);
	fake_advance(CALL_XFER_BYE_GRACE_MS);
	CHECK_UINT(count_send("BYE"), 1);
	CHECK(req_of("BYE", &ri));
	CHECK(!strcmp(ri.to_tag, "peer-a"));

	/*
	 * The Metaswitch order: an interim NOTIFY, a c=0.0.0.0 re-INVITE on the
	 * replaced leg that the switch's own BYE overtakes, the final NOTIFY
	 * with reason=timeout, and the switch's BYE on the referee 1 ms behind
	 * it, crossing the BYE this side sends on the verdict.
	 */
	CASE("consult: an interim NOTIFY is progress, answered 200 with the guards standing");
	consult_accepted();
	rx(consult_notify(2, "active;expires=3600", "SIP/2.0 100 Trying\r\n"));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(find_send("BYE") < 0);
	CHECK_UINT(timer_count(SIP_TIMER_CALL_C), 2);

	CASE("consult: a c=0.0.0.0 re-INVITE on the drain leg is answered inactive");
	rx(uac_in_dialog("INVITE", "peer-b", 2, NULL, sdp_body("0.0.0.0", 29662, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(has(last_text(), "a=inactive"));

	CASE("consult: the network BYE that overtakes that answer ends the leg, no abandon");
	fake_advance(400);
	rx(uac_in_dialog("BYE", "peer-b", 3, NULL, NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(find_send("BYE") < 0);
	CHECK_UINT(ev.ends[0][1].count, 1);
	CHECK(ev.ends[0][1].transferred);

	/* the record dates its clear at the silence, the 202, and not at the
	 * network BYE that ends the dialog: the drain is protocol, not talk */
	CASE("consult: the replaced leg's record dates its clear at the 202, not at the BYE");
	CHECK(ev.end.cleared);
	CHECK(ev.end.cleared_ago_ms >= 400);

	CASE("consult: the final NOTIFY then graces the referee, no BYE of ours yet");
	rx(consult_notify(3, "terminated;reason=timeout", "SIP/2.0 200 OK\r\n"));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(find_send("BYE") < 0);
	CHECK_UINT((unsigned int)timer_delay(SIP_TIMER_CALL_C), CALL_XFER_BYE_GRACE_MS);

	CASE("consult: and the switch's BYE 1 ms behind it ends the leg with no BYE of ours");
	fake_advance(1);
	rx(referee_in_dialog("BYE",4, NULL, NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(ev.remote_bye, 0);
	CHECK_UINT(ev.ends[0][0].count, 1);
	CHECK(ev.ends[0][0].transferred);
	CHECK(find_send("BYE") < 0);
	CHECK(!timer_armed(SIP_TIMER_CALL_C));
	fake_advance(CALL_XFER_BYE_GRACE_MS);
	CHECK(find_send("BYE") < 0);
	CHECK(slot_is(0));

	/*
	 * A 2xx the peer never ACKs abandons an ordinary leg. A draining one is
	 * bounded by its guards, and the abandon would BYE the dialog the drain
	 * keeps for the network's own BYE; the leg returns to ACTIVE instead
	 * and keeps taking the peer's re-INVITEs.
	 */
	CASE("consult: a 2xx the peer never ACKs on the drain leg draws no BYE from this side");
	consult_accepted();
	rx(consult_notify(2, "terminated;reason=noresource", "SIP/2.0 200 OK\r\n"));
	fake_advance(CALL_XFER_BYE_GRACE_MS);
	reply(200, NULL, NULL, NULL);
	fake_advance(6000);
	rx(uac_in_dialog("INVITE", "peer-b", 2, NULL, sdp_body(T_PEER, 20002, "0 101", NULL)));
	CHECK(has(last_text(), "SIP/2.0 200"));
	fake_advance(33000);
	CHECK_UINT(count_send("BYE"), 1);
	CHECK_UINT(timer_count(SIP_TIMER_CALL_C), 1);

	CASE("consult: and that leg takes the next re-INVITE again");
	rx(uac_in_dialog("INVITE", "peer-b", 3, NULL,
			 sdp_body(T_PEER, 20002, "0 101", "a=recvonly\r\n")));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(has(last_text(), "a=sendonly"));

	/*
	 * Three offers keep their refusals on a draining leg. An offerless one
	 * would put our offer in the 2xx, and an ACK without the answer that
	 * asks for abandons a leg above the name gate. An image-only one has no
	 * audio to answer once T.38 is off, and a stream nobody programs must
	 * not be accepted. A session interval below the floor meets the 422
	 * every re-INVITE meets.
	 */
	CASE("consult: an offerless re-INVITE on the drain leg keeps the 488");
	consult_accepted();
	rx(consult_notify(2, "terminated;reason=noresource", "SIP/2.0 200 OK\r\n"));
	fake_advance(CALL_XFER_BYE_GRACE_MS);
	reply(200, NULL, NULL, NULL);
	rx(uac_in_dialog("INVITE", "peer-b", 2, NULL, NULL));
	CHECK(has(last_text(), "SIP/2.0 488"));
	CHECK_UINT(count_send("BYE"), 1);

	CASE("consult: an image-only offer on the drain leg is refused 488");
	rx(uac_in_dialog("INVITE", "peer-b", 3, NULL,
			 "v=0\r\n"
			 "o=- 1 2 IN IP4 " T_PEER "\r\n"
			 "s=-\r\n"
			 "c=IN IP4 " T_PEER "\r\n"
			 "t=0 0\r\n"
			 "m=image 30002 udptl t38\r\n"
			 "a=T38FaxVersion:0\r\n"));
	CHECK(has(last_text(), "SIP/2.0 488"));

	CASE("consult: a re-INVITE below the session floor on the drain leg is refused 422");
	rx(uac_in_dialog("INVITE", "peer-b", 4, "Session-Expires: 60;refresher=uac\r\n",
			 sdp_body(T_PEER, 20002, "0 101", NULL)));
	CHECK(has(last_text(), "SIP/2.0 422"));
	CHECK_UINT(count_send("BYE"), 1);

	CASE("consult: and the drain leg still answers the network BYE behind all three");
	rx(uac_in_dialog("BYE", "peer-b", 5, NULL, NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(count_send("BYE"), 1);

	/*
	 * A session timer the replaced leg negotiated before the transfer
	 * stops at the 202 and rides no drain answer: this side will neither
	 * refresh nor watch, so the 2xx must not agree to a refresh.
	 */
	CASE("consult: a drain answer carries no session timer the leg negotiated before");
	fx_reset();
	CHECK(dial(0, 0, "1002"));
	reply(100, NULL, NULL, NULL);
	reply(200, "peer-a", NULL, sdp_body(T_PEER, 20000, "0 101", NULL));
	CHECK(call_hold(0, 0));
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "0 101", "a=recvonly\r\n"));
	CHECK(dial(0, 1, "1003"));
	reply(100, NULL, NULL, NULL);
	reply(200, "peer-b",
	      "Contact: <sip:1003@203.0.113.9:5062>\r\n"
	      "Session-Expires: 1800;refresher=uas\r\n",
	      sdp_body(T_PEER, 20002, "0 101", NULL));
	CHECK(timer_armed(SIP_TIMER_CALL_WATCHDOG));
	CHECK(call_xfer_consult(0, 0, 1));
	reply(202, NULL, NULL, NULL);
	CHECK(!timer_armed(SIP_TIMER_CALL_WATCHDOG));
	rx(uac_in_dialog("INVITE", "peer-b", 2, "Supported: timer\r\n",
			 sdp_body(T_PEER, 20002, "0 101", NULL)));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(!has(last_text(), "Session-Expires"));
	CHECK(!has(last_text(), "Require: timer"));

	/*
	 * The replaced leg may carry an exchange of its own when the 202 lands:
	 * call_xfer_consult() admits it in REINVITE_SENT. The drain leaves that
	 * exchange to end on its own with nothing counted: the 2xx that answers
	 * it is still ACKed (RFC 3261 13.2.2.4), but it programs no stream and
	 * arms no session timer, and the leg returns to ACTIVE so that it keeps
	 * answering the peer's re-INVITEs instead of refusing them 491.
	 */
	CASE("consult: an exchange in flight on the replaced leg is ACKed and nothing "
	     "else of it counts");
	consult_up();
	CHECK(call_hold(0, 1));
	{
		int ridx = find_send("INVITE");
		unsigned int starts, dones, acks;

		CHECK(call_xfer_consult(0, 0, 1));
		reply_at((unsigned int)find_send("REFER"), 202, NULL, NULL, NULL);
		CHECK(find_send("BYE") < 0);
		starts = med.start_calls;
		dones = ev.reinvite_done;
		acks = count_send("ACK");
		reply_at((unsigned int)ridx, 200, NULL, NULL,
			 sdp_body(T_PEER, 20002, "0 101", "a=recvonly\r\n"));
		CHECK_UINT(count_send("ACK"), acks + 1);
		CHECK_UINT(med.start_calls, starts);
		CHECK(!timer_armed(SIP_TIMER_CALL_SESSION));
		CHECK(!timer_armed(SIP_TIMER_CALL_WATCHDOG));
		CHECK_UINT(ev.reinvite_done, dones);
		CHECK(find_send("BYE") < 0);
	}

	CASE("consult: and the leg answers the peer's next re-INVITE 200 sendonly, not 491");
	rx(uac_in_dialog("INVITE", "peer-b", 2, NULL, sdp_body(T_PEER, 20002, "0 101", NULL)));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(has(last_text(), "a=sendonly"));
	rx(uac_in_dialog("ACK", "peer-b", 2, NULL, NULL));
	rx(uac_in_dialog("BYE", "peer-b", 3, NULL, NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(find_send("BYE") < 0);

	CASE("consult: a challenge to that exchange starts no fresh INVITE on the "
	     "draining leg");
	consult_up();
	CHECK(call_hold(0, 1));
	{
		int ridx = find_send("INVITE");
		unsigned int invites;

		CHECK(call_xfer_consult(0, 0, 1));
		reply_at((unsigned int)find_send("REFER"), 202, NULL, NULL, NULL);
		invites = count_send("INVITE");
		reply_at((unsigned int)ridx, 401, NULL,
			 "WWW-Authenticate: Digest realm=\"" T_DOMAIN "\", nonce=\"" T_NONCE "\"\r\n",
			 NULL);
		CHECK_UINT(count_send("INVITE"), invites);
		CHECK(find_send("BYE") < 0);
	}

	CASE("consult: and that leg too answers the peer's next re-INVITE 200, not 491");
	rx(uac_in_dialog("INVITE", "peer-b", 2, NULL, sdp_body(T_PEER, 20002, "0 101", NULL)));
	CHECK(has(last_text(), "SIP/2.0 200"));
	rx(uac_in_dialog("BYE", "peer-b", 3, NULL, NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(find_send("BYE") < 0);

	CASE("consult: an exchange whose transaction dies on the draining leg draws no BYE");
	consult_up();
	CHECK(call_hold(0, 1));
	CHECK(call_xfer_consult(0, 0, 1));
	reply_at((unsigned int)find_send("REFER"), 202, NULL, NULL, NULL);
	rx(consult_notify(2, "terminated;reason=noresource", "SIP/2.0 200 OK\r\n"));
	fake_advance(CALL_XFER_BYE_GRACE_MS);	/* the referee BYE leaves */
	reply(200, NULL, NULL, NULL);		/* and completes */
	CHECK_UINT(count_send("BYE"), 1);
	fake_advance(33000);
	CHECK_UINT(count_send("BYE"), 1);
	CHECK_UINT(timer_count(SIP_TIMER_CALL_C), 1);
	rx(uac_in_dialog("INVITE", "peer-b", 2, NULL, sdp_body(T_PEER, 20002, "0 101", NULL)));
	CHECK(has(last_text(), "SIP/2.0 200"));

	/*
	 * A failure verdict BYEs a replaced leg whose own exchange is still
	 * out. The final that answers it later finds the BYE on the wire and
	 * leaves that state alone: only a leg still in REINVITE_SENT returns to
	 * ACTIVE, and a BYE_SENT leg answers a peer re-INVITE 481, the dialog
	 * its BYE ended (RFC 3261 15.1.1).
	 */
	CASE("consult: a failure verdict BYEs a replaced leg whose exchange is still out");
	consult_up();
	CHECK(call_hold(0, 1));
	{
		int ridx = find_send("INVITE");

		CHECK(call_xfer_consult(0, 0, 1));
		reply_at((unsigned int)find_send("REFER"), 202, NULL, NULL, NULL);
		rx(consult_notify(2, "terminated;reason=noresource", "SIP/2.0 486 Busy Here\r\n"));
		CHECK_UINT(count_send("BYE"), 2);
		reply_at((unsigned int)ridx, 200, NULL, NULL,
			 sdp_body(T_PEER, 20002, "0 101", "a=recvonly\r\n"));
		CHECK(slot_view_get(0, 1, &sv));
		CHECK(!strcmp(sv.state, "bye-sent"));
	}

	CASE("consult: and a peer re-INVITE behind that BYE is answered 481, not 491");
	rx(uac_in_dialog("INVITE", "peer-b", 2, NULL, sdp_body(T_PEER, 20002, "0 101", NULL)));
	CHECK(has(last_text(), "SIP/2.0 481"));

	/*
	 * The referee may be waiting for the ACK of an offerless exchange it
	 * accepted before the 202. An ACK without the answer that 2xx asked for
	 * abandons an ordinary leg; a draining one drops the offer and keeps
	 * the dialog, because the verdict decides it.
	 */
	CASE("consult: an ACK without the answer our 2xx asked for does not abandon the referee");
	consult_up();
	rx(referee_in_dialog("INVITE",2, NULL, NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(call_xfer_consult(0, 0, 1));
	reply(202, NULL, NULL, NULL);
	rx(referee_in_dialog("ACK",2, NULL, NULL));
	CHECK(find_send("BYE") < 0);
	CHECK_UINT(ev.failed, 0);
	rx(consult_notify(3, "terminated;reason=noresource", "SIP/2.0 200 OK\r\n"));
	CHECK(find_send("BYE") < 0);
	fake_advance(CALL_XFER_BYE_GRACE_MS);
	CHECK_UINT(count_send("BYE"), 1);
	CHECK(req_of("BYE", &ri));
	CHECK(!strcmp(ri.to_tag, "peer-a"));

	CASE("consult: a REFER whose transaction dies BYEs both legs, nothing was silenced");
	consult_up();
	CHECK(call_xfer_consult(0, 0, 1));
	CHECK_UINT(med.stop_calls, 0);
	/* to timer F exactly: a step past it would count the UDP
	 * retransmissions of the two unanswered BYEs */
	fake_advance(32000);
	CHECK_UINT(count_send("BYE"), 2);
	CHECK_UINT(ev.xfer_done, 0);
	CHECK_UINT(timer_count(SIP_TIMER_CALL_C), 0);

	/* the referee BYE goes first (xfer_leg_clear) and the replaced leg's
	 * behind it, so the last BYE is the one that rides the replaced dialog */
	CASE("consult: a 403 to the REFER BYEs both legs with nothing silenced");
	consult_up();
	CHECK(call_xfer_consult(0, 0, 1));
	CHECK_UINT(med.stop_calls, 0);
	reply(403, NULL, NULL, NULL);
	CHECK_UINT(count_send("BYE"), 2);
	CHECK(req_info_at((unsigned int)find_send("BYE") - 1, &ri));
	CHECK(!strcmp(ri.to_tag, "peer-a"));
	CHECK(req_info_at((unsigned int)find_send("BYE"), &ri));
	CHECK(!strcmp(ri.to_tag, "peer-b"));

	CASE("consult: the consult kind is refused by the single-leg entry");
	consult_up();
	CHECK(!call_xfer(0, 0, CALL_XFER_CONSULT, "1003"));
	CHECK(find_send("REFER") < 0);

	/*
	 * The replaced leg is named by a handle, not by a slot. The subscriber
	 * of a consult transfer is already on hook, so a replaced leg that
	 * dies during the wait frees a slot a new inbound call can take and
	 * ring on. A slot index resolved at outcome time would BYE that new
	 * call; the generation in the handle says the leg this transfer meant
	 * is gone.
	 */
	CASE("consult: a replaced leg that died frees a slot a new call can take");
	consult_accepted();
	rx(uac_in_dialog("BYE", "peer-b", 2, NULL, NULL));
	CHECK_UINT(ev.remote_bye, 0);
	CHECK(slot_is(1));
	rx(invite_text("call-xc-new", NULL, sdp_body(T_PEER, 20004, "0", NULL)));
	CHECK_UINT(ev.incoming, 1);
	CHECK_UINT(ev.slot, 1);
	CHECK(call_ring(0, 1));
	CHECK(has(last_text(), "SIP/2.0 180"));

	CASE("consult: and the verdict guard BYEs the referee alone, that new call rings on");
	fake_advance(CALL_XFER_CONSULT_WAIT_MS);
	CHECK_UINT(count_send("BYE"), 1);
	CHECK(req_of("BYE", &ri));
	CHECK(!strcmp(ri.to_tag, "peer-a"));
	CHECK(slot_view_get(0, 1, &sv));
	CHECK(!strcmp(sv.state, "ringing-in"));
	CHECK(sv.ui_busy);
	CHECK_UINT(ev.xfer_done, 0);

	/*
	 * A credential retry rebuilds the request, and a consult that lost its
	 * replaced leg meanwhile has no Replaces left to embed. Rebuilding
	 * without it would put a blind transfer to the same target on the wire:
	 * the subscriber asked to join two calls and would get one handed away.
	 */
	CASE("consult: a retry that lost the replaced leg fails instead of going blind");
	consult_up();
	CHECK(call_xfer_consult(0, 0, 1));
	rx(uac_in_dialog("BYE", "peer-b", 2, NULL, NULL));
	CHECK_UINT(ev.remote_bye, 1);
	CHECK_UINT(ev.slot, 1);
	refers = count_send("REFER");
	reply_at((unsigned int)find_send("REFER"), 401, NULL,
		 "WWW-Authenticate: Digest realm=\"" T_DOMAIN "\", nonce=\"" T_NONCE "\"\r\n",
		 NULL);
	CHECK_UINT(count_send("REFER"), refers);
	CHECK_UINT(ev.xfer_done, 0);

	CASE("consult: and the referee is cleared, the form BYEs on every failure");
	CHECK_UINT(count_send("BYE"), 1);
	CHECK(slot_is(1));

	CASE("consult: a retry whose slot a NEW call took fails on the handle");
	consult_up();
	CHECK(call_xfer_consult(0, 0, 1));
	rx(uac_in_dialog("BYE", "peer-b", 2, NULL, NULL));
	rx(invite_text("call-xc-new2", NULL, sdp_body(T_PEER, 20004, "0", NULL)));
	CHECK_UINT(ev.slot, 1);
	CHECK(call_ring(0, 1));
	refers = count_send("REFER");
	byes = count_send("BYE");
	reply_at((unsigned int)find_send("REFER"), 401, NULL,
		 "WWW-Authenticate: Digest realm=\"" T_DOMAIN "\", nonce=\"" T_NONCE "\"\r\n",
		 NULL);
	CHECK_UINT(count_send("REFER"), refers);
	CHECK_UINT(ev.xfer_done, 0);

	CASE("consult: and that new call is untouched by the failure too");
	CHECK_UINT(count_send("BYE"), byes + 1);
	CHECK(slot_view_get(0, 1, &sv));
	CHECK(!strcmp(sv.state, "ringing-in"));

	/*
	 * RFC 3261 25.1: the first URI header opens with '?' and every one
	 * behind it with '&'. A peer Contact that already carries one would
	 * otherwise get a second '?' and name a URI no proxy can parse.
	 */
	CASE("consult: a target that already carries a URI header takes '&Replaces='");
	fx_reset();
	CHECK(dial(0, 0, "1002"));
	reply(100, NULL, NULL, NULL);
	reply(200, "peer-a", NULL, sdp_body(T_PEER, 20000, "0 101", NULL));
	CHECK(dial(0, 1, "1003"));
	reply(100, NULL, NULL, NULL);
	reply(200, "peer-b", "Contact: <sip:1003@203.0.113.9:5062?X-Info=1>\r\n",
	      sdp_body(T_PEER, 20002, "0 101", NULL));
	CHECK(call_xfer_consult(0, 0, 1));
	CHECK(has(last_text(), "Refer-To: <sip:1003@203.0.113.9:5062?X-Info=1&Replaces="));
	CHECK(!has(last_text(), "?X-Info=1?Replaces="));

	/* the same refuse-not-truncate rule as the dialled target, at the one
	 * target that arrives from the wire */
	CASE("consult: a remote target too wide for a request URI is refused");
	fx_reset();
	CHECK(dial(0, 0, "1002"));
	reply(100, NULL, NULL, NULL);
	reply(200, "peer-a", NULL, sdp_body(T_PEER, 20000, "0 101", NULL));
	CHECK(dial(0, 1, "1003"));
	reply(100, NULL, NULL, NULL);
	{
		char contact[512];
		char user[400];

		memset(user, 'u', sizeof(user) - 1);
		user[sizeof(user) - 1] = 0;
		snprintf(contact, sizeof(contact), "Contact: <sip:%s@203.0.113.9:5062>\r\n",
			 user);
		reply(200, "peer-b", contact, sdp_body(T_PEER, 20002, "0 101", NULL));
	}
	CHECK(!call_xfer_consult(0, 0, 1));
	CHECK(find_send("REFER") < 0);
	CHECK_UINT(ev.xfer_done, 0);

	/* two transfers racing for one dialog: the leg being handed over is
	 * already being handed somewhere else */
	CASE("consult: a replaced leg with a transfer of its own is refused");
	consult_up();
	CHECK(call_xfer(0, 1, CALL_XFER_ATTENDED, "1004"));
	reply(202, NULL, NULL, NULL);
	CHECK(!call_xfer_consult(0, 0, 1));
	CHECK_UINT(count_send("REFER"), 1);
	CHECK(find_send("BYE") < 0);

	CASE("consult: a replaced slot that holds no leg is refused");
	consult_up();
	CHECK(!call_xfer_consult(0, 0, CALL_SLOTS));
	CHECK(find_send("REFER") < 0);
	CHECK_UINT(ev.xfer_done, 0);

	CASE("consult: the same slot twice is refused");
	CHECK(!call_xfer_consult(0, 0, 0));
	CHECK(find_send("REFER") < 0);

	CASE("consult: a referee with no confirmed dialog is refused");
	fx_reset();
	CHECK(dial(0, 0, "1002"));
	reply(100, NULL, NULL, NULL);
	reply(180, "peer-a", NULL, NULL);
	CHECK(dial(0, 1, "1003"));
	reply(100, NULL, NULL, NULL);
	reply(200, "peer-b", NULL, sdp_body(T_PEER, 20002, "0 101", NULL));
	CHECK(!call_xfer_consult(0, 0, 1));
	CHECK(find_send("REFER") < 0);

	CASE("consult: a replaced leg whose dialog is not confirmed is refused");
	fx_reset();
	CHECK(dial(0, 0, "1002"));
	reply(100, NULL, NULL, NULL);
	reply(200, "peer-a", NULL, sdp_body(T_PEER, 20000, "0 101", NULL));
	rx(invite_text("call-xc1", NULL, sdp_body(T_PEER, 20002, "0", NULL)));
	CHECK_UINT(ev.incoming, 1);
	CHECK_UINT(ev.slot, 1);
	CHECK(call_ring(0, 1));
	CHECK(!call_xfer_consult(0, 0, 1));
	CHECK(find_send("REFER") < 0);
	CHECK_UINT(ev.xfer_done, 0);

	CASE("consult: a refusal clears neither leg, which is line/line.c's to do");
	CHECK(find_send("BYE") < 0);
	CHECK_UINT(ev.failed, 0);
	{
		unsigned int slot = 99;

		CHECK(!call_slot_free(0, &slot));
	}
}

/*
 * The on-hook blind form (call.h CALL_XFER_ONHOOK): the REFER and the 202
 * outcome of the blind form, and one difference, the failure branch. The
 * subscriber is on hook. A refused referral therefore BYEs the leg, and does
 * not leave it for a machine that has nothing to resume it with.
 */
static void test_xfer_onhook(void)
{
	CASE("onhook: the REFER is the blind one, a bare target and no Replaces");
	fx_reset();
	uac_up(NULL);
	CHECK(call_xfer(0, 0, CALL_XFER_ONHOOK, "1003"));
	CHECK_UINT(count_send("REFER"), 1);
	CHECK(has(last_text(), "Refer-To: <sip:1003@" T_DOMAIN ">\r\n"));
	CHECK(!has(last_text(), "Replaces"));
	CHECK(find_send("BYE") < 0);

	CASE("onhook: the 202 reports ok and drains the leg, as the blind form does");
	reply(202, NULL, NULL, NULL);
	CHECK(find_send("BYE") < 0);
	CHECK_UINT(ev.xfer_done, 1);
	CHECK(ev.xfer_ok);
	CHECK_UINT(ev.xfer_code, 202);
	CHECK(timer_armed(SIP_TIMER_CALL_C));
	CHECK_UINT(timer_delay(SIP_TIMER_CALL_C), CALL_XFER_WAIT_MS);

	/* the drain runs for the whole guard, so the occupancy goes with the
	 * report: a line held that long would refuse every inbound call for a
	 * subscriber who is already on hook (call.h CALL_XFER_ONHOOK) */
	CASE("onhook: the leg gives its UI occupancy up at the 202");
	{
		struct slot_view sv;

		CHECK(slot_view_get(0, 0, &sv));
		CHECK(!strcmp(sv.state, "active"));
		CHECK(!sv.ui_busy);
		CHECK(sv.shadow);
	}

	CASE("onhook: so the line takes an inbound call on the free slot while it drains");
	CHECK(slot_is(1));
	CHECK(!call_hangup(0, 0));
	CHECK(find_send("BYE") < 0);

	CASE("onhook: the switch's BYE ends the drain and the record carries the transfer key");
	rx(uac_in_dialog("BYE", "peer-tag", 2, NULL, NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(ev.ends[0][0].count, 1);
	CHECK(ev.ends[0][0].transferred);
	CHECK_UINT(ev.remote_bye, 0);
	CHECK(slot_is(0));

	CASE("onhook: a verdict of failure behind the 202 BYEs the leg and reports nothing");
	fx_reset();
	uac_up(NULL);
	CHECK(call_xfer(0, 0, CALL_XFER_ONHOOK, "1003"));
	reply(202, NULL, NULL, NULL);
	rx(xfer_notify(2, 2, "terminated;reason=noresource", "SIP/2.0 486 Busy Here\r\n"));
	CHECK_UINT(count_send("BYE"), 1);
	CHECK_UINT(ev.xfer_done, 1);
	CHECK(ev.xfer_ok);

	CASE("onhook: a refused REFER BYEs the leg too, unlike the blind form");
	fx_reset();
	uac_up(NULL);
	CHECK(call_xfer(0, 0, CALL_XFER_ONHOOK, "1003"));
	reply(603, NULL, NULL, NULL);
	CHECK_UINT(count_send("BYE"), 1);
	CHECK_UINT(ev.xfer_done, 1);
	CHECK(!ev.xfer_ok);
	CHECK_UINT(ev.xfer_code, 603);

	/* the IDLE cell answers the failure with a hangup of its own (fsm.c
	 * st_idle); the leg is already BYEd and keeps its occupancy */
	CASE("onhook: the machine's hangup of the failed leg leaves the occupancy alone");
	{
		struct slot_view sv;

		CHECK(call_hangup(0, 0));
		CHECK_UINT(count_send("BYE"), 1);
		CHECK(slot_view_get(0, 0, &sv));
		CHECK(!strcmp(sv.state, "bye-sent"));
		CHECK(sv.ui_busy);
	}

	CASE("onhook: the machine hears no clearing class, and the record carries "
	     "no transfer key");
	CHECK_UINT(ev.failed, 0);
	CHECK_UINT(ev.remote_bye, 0);
	reply(200, NULL, NULL, NULL);
	fake_advance(6000);
	CHECK_UINT(ev.ends[0][0].count, 1);
	CHECK(!ev.ends[0][0].transferred);
	CHECK(slot_is(0));

	CASE("onhook: a REFER that dies unanswered BYEs the leg as well");
	fx_reset();
	uac_up(NULL);
	CHECK(call_xfer(0, 0, CALL_XFER_ONHOOK, "1003"));
	fake_advance(40000);
	CHECK_UINT(ev.xfer_done, 1);
	CHECK(!ev.xfer_ok);
	CHECK_UINT(ev.xfer_code, 408);
	CHECK(find_send("BYE") >= 0);

	CASE("onhook: a challenged REFER is resent with credentials and no Replaces");
	fx_reset();
	uac_up(NULL);
	CHECK(call_xfer(0, 0, CALL_XFER_ONHOOK, "1003"));
	reply(401, NULL,
	      "WWW-Authenticate: Digest realm=\"" T_DOMAIN "\", nonce=\"" T_NONCE "\"\r\n",
	      NULL);
	CHECK_UINT(count_send("REFER"), 2);
	CHECK(has(last_text(), "Authorization: Digest"));
	CHECK(!has(last_text(), "Replaces"));
	CHECK_UINT(ev.xfer_done, 0);
	CHECK(find_send("BYE") < 0);
	reply(202, NULL, NULL, NULL);
	CHECK(find_send("BYE") < 0);
	CHECK(timer_armed(SIP_TIMER_CALL_C));
	CHECK_UINT(ev.xfer_done, 1);
	CHECK(ev.xfer_ok);

	CASE("onhook: a verdict 2xx graces the drained leg and the grace ends it, once");
	fx_reset();
	uac_up(NULL);
	CHECK(call_xfer(0, 0, CALL_XFER_ONHOOK, "1003"));
	reply(202, NULL, NULL, NULL);
	rx(xfer_notify(2, 2, "terminated;reason=timeout", "SIP/2.0 200 OK\r\n"));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(find_send("BYE") < 0);
	CHECK_UINT((unsigned int)timer_delay(SIP_TIMER_CALL_C), CALL_XFER_BYE_GRACE_MS);
	fake_advance(CALL_XFER_BYE_GRACE_MS - 1);
	CHECK(find_send("BYE") < 0);
	fake_advance(1);
	CHECK_UINT(count_send("BYE"), 1);
	CHECK_UINT(ev.xfer_done, 1);

	/* the drain that BYEd itself is the BYE_SENT predecessor a dial has
	 * always reclaimed in place: its port comes back before the new call
	 * asks for one */
	CASE("onhook: a dial on that name reclaims the slot in place once its BYE is out");
	{
		struct slot_view sv;

		CHECK(dial(0, 0, "1004"));
		CHECK(has(last_text(), "INVITE sip:1004@" T_DOMAIN));
		CHECK_UINT(count_send("BYE"), 1);
		CHECK(slot_view_get(0, 0, &sv));
		CHECK(!strcmp(sv.state, "invite-sent"));
		CHECK(!sv.shadow);
		CHECK(slot_is(1));
	}
}

/* --------------------------------------------------- the transferee's REFER */

/*
 * The transferee side of RFC 3515: this line is referred somewhere else.
 * Every case drives the leg layer alone. What is asserted is the wire, the
 * child INVITE the referral produced, and above all the silence: the machine
 * must learn nothing at all about the exchange, so most of these cases are
 * counts of callbacks that did not fire.
 */

/*
 * A request from the peer inside the first call this fixture placed. A
 * transferee case has two outbound INVITEs on the wire, so uac_in_dialog(),
 * which reads the last one, would build the request against the child's dialog
 * instead of the one the referral arrived on.
 */
static const char *xfer_parent_req(const char *method, unsigned int cseq,
				   const char *extra, const char *body)
{
	struct req_info ri;

	if (!req_info_at(0, &ri))
		return "";

	return dialog_req_text(method, cseq, ri.call_id, "peer-tag", ri.from_tag, extra,
			       body);
}

/* an in-dialog REFER from the peer of the call uac_up() placed */
static const char *refer_text(unsigned int cseq, const char *refer_to, const char *extra)
{
	char hdrs[1024];

	snprintf(hdrs, sizeof(hdrs), "Refer-To: %s\r\n%s", refer_to, extra ? extra : "");

	return xfer_parent_req("REFER", cseq, hdrs, NULL);
}

/*
 * An answered outbound call that has just been referred somewhere else. The
 * callback recorder is cleared between the two, so every count a case makes
 * afterwards is a count of what the referral did.
 */
static void refer_up(const char *refer_to)
{
	fx_reset();
	uac_up(NULL);
	memset(&ev, 0, sizeof(ev));
	rx(refer_text(2, refer_to, NULL));
}

/*
 * The child INVITE, which is the first one carrying a Call-ID the referred call
 * does not: counting INVITEs would find the parent's own hold re-INVITE, and
 * the target's contact is the fixture's, so the request URI cannot name it
 * either.
 */
static int child_invite_at(void)
{
	unsigned int i, n = fk.sends < FAKE_SEND_MAX ? fk.sends : FAKE_SEND_MAX;
	struct req_info ri, parent;
	bool placed = req_info_at(0, &parent);

	/* a call this line answered sent no INVITE of its own, so every one on
	 * the wire is the referral's */
	for (i = 0; i < n; i++) {
		if (!req_info_at(i, &ri) || strcmp(ri.method, "INVITE"))
			continue;
		if (!placed || strcmp(ri.call_id, parent.call_id))
			return (int)i;
	}

	return -1;
}

static bool child_invite(struct req_info *ri)
{
	int idx = child_invite_at();

	return idx >= 0 && req_info_at((unsigned int)idx, ri);
}

/* the target answers the child INVITE the referral placed */
static void child_answer(void)
{
	int idx = child_invite_at();

	if (idx >= 0)
		reply_at((unsigned int)idx, 200, "target-tag", NULL,
			 sdp_body(T_PEER, 20002, "0 101", NULL));
}

/*
 * RFC 3608. Every request that leaves the dialog that an INVITE creates
 * carries the route that the registrar granted, and no other request does.
 * The next hop is the sticky registrar target throughout: the hop names are
 * never resolved (sip/reg.h).
 */
static void test_service_route(void)
{
	const char *chal = "WWW-Authenticate: Digest realm=\"" T_DOMAIN
			   "\", nonce=\"" T_NONCE "\"\r\n";
	const char *rr = "Record-Route: <sip:rr.example.net;lr>\r\n";
	struct req_info ri;
	int idx;

	CASE("service route: an INVITE without one carries no Route header");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	CHECK(!has(send_text(0), "\r\nRoute:"));
	CHECK_UINT(count_hdr(send_text(0), "Route:"), 0);

	CASE("service route: an INVITE with one carries its hops in order");
	fx_reset();
	fk.service_route = sr_take(T_SR_HDRS);
	CHECK(call_dial(0, 0, "1002", true));
	CHECK(has(send_text(0), T_SR_TEXT));
	CHECK_UINT(count_hdr(send_text(0), "Route:"), 2);
	/* the position on the wire, which uas_service_route.xml and the e2e
	 * reader depend on: the hops open the line after CSeq and the Contact
	 * follows them */
	CHECK(has(send_text(0), "\r\nCSeq: 1 INVITE\r\n" T_SR_TEXT "Contact:"));

	CASE("service route: the credential retry keeps them");
	reply(100, NULL, NULL, NULL);
	reply(401, "peer-tag", chal, NULL);
	CHECK_UINT(count_send("INVITE"), 2);
	CHECK(has(last_text(), "Authorization: Digest"));
	CHECK(has(last_text(), T_SR_TEXT));
	CHECK_UINT(count_hdr(last_text(), "Route:"), 2);

	/*
	 * The leg copies the block at start and does not read the view again,
	 * so a registration refresh that replaces the registrar's route under
	 * a running call cannot re-aim its retries. sr_take() writes one
	 * static block, exactly as reg.c replaces the stored route in place.
	 */
	CASE("service route: a route the registration replaced leaves the "
	     "retries alone");
	fx_reset();
	fk.service_route = sr_take(T_SR_HDRS);
	CHECK(call_dial(0, 0, "1002", true));
	reply(100, NULL, NULL, NULL);
	sr_take(T_SR_OTHER);
	reply(401, "peer-tag", chal, NULL);
	CHECK_UINT(count_send("INVITE"), 2);
	CHECK(has(last_text(), T_SR_TEXT));
	CHECK(!has(last_text(), "far.example.org"));

	/*
	 * The ACK of a non-2xx is built from the request the transaction
	 * retained, so it carries the route without a reader of its own
	 * (req_mirror_build). The 401 above is that final.
	 */
	CASE("service route: the ACK for a non-2xx carries them");
	idx = find_send("ACK");
	CHECK(idx >= 0);
	CHECK(has(send_text((unsigned int)idx), T_SR_TEXT));
	CHECK_UINT(count_hdr(send_text((unsigned int)idx), "Route:"), 2);

	CASE("service route: the 422 retry keeps them");
	fx_reset();
	cfg.channel[0].session_max_expires = 600;
	fk.service_route = sr_take(T_SR_HDRS);
	CHECK(call_dial(0, 0, "1002", true));
	reply(100, NULL, NULL, NULL);
	reply(422, "peer-tag", "Min-SE: 900\r\n", NULL);
	CHECK_UINT(count_send("INVITE"), 2);
	CHECK(has(last_text(), "Session-Expires: 900"));
	CHECK(has(last_text(), T_SR_TEXT));
	cfg.channel[0].session_max_expires = 0;

	CASE("service route: the 491 retry keeps them");
	fx_reset();
	fk.service_route = sr_take(T_SR_HDRS);
	CHECK(call_dial(0, 0, "1002", true));
	reply(100, NULL, NULL, NULL);
	reply(491, "peer-tag", NULL, NULL);
	fake_advance(4000);
	CHECK_UINT(count_send("INVITE"), 2);
	CHECK(last_req(&ri));
	CHECK_UINT(ri.cseq, 2);
	CHECK(has(last_text(), T_SR_TEXT));

	CASE("service route: the CANCEL of that INVITE carries them");
	fx_reset();
	fk.service_route = sr_take(T_SR_HDRS);
	CHECK(call_dial(0, 0, "1002", true));
	reply(100, NULL, NULL, NULL);
	CHECK(call_hangup(0, 0));
	idx = find_send("CANCEL");
	CHECK(idx >= 0);
	CHECK(has(send_text((unsigned int)idx), T_SR_TEXT));
	CHECK_UINT(count_hdr(send_text((unsigned int)idx), "Route:"), 2);

	CASE("service route: the transferred call's child INVITE carries them");
	fx_reset();
	fk.service_route = sr_take(T_SR_HDRS);
	uac_up(NULL);
	memset(&ev, 0, sizeof(ev));
	rx(refer_text(2, "<sip:1003@" T_DOMAIN ">", NULL));
	idx = child_invite_at();
	CHECK(idx >= 0);
	CHECK(child_invite(&ri));
	CHECK(!strcmp(ri.ruri, "sip:1003@" T_DOMAIN));
	CHECK(has(send_text((unsigned int)idx), T_SR_TEXT));

	/*
	 * An in-dialog request takes the route set of RFC 3261 12.1.2, which
	 * dialog.c builds from Record-Route alone. The registration route has
	 * no place there: the dialog the peer's answer established says where
	 * its own requests go.
	 */
	CASE("service route: the in-dialog BYE carries the Record-Route set "
	     "instead");
	fx_reset();
	fk.service_route = sr_take(T_SR_HDRS);
	CHECK(call_dial(0, 0, "1002", true));
	reply(100, NULL, NULL, NULL);
	/* the 18x creates the dialog, so it is the answer whose Record-Route
	 * becomes the route set (RFC 3261 12.1.2) */
	reply(180, "peer-tag", rr, NULL);
	reply(200, "peer-tag", rr, sdp_body(T_PEER, 20000, "0 101", NULL));
	CHECK(call_hangup(0, 0));
	idx = find_send("BYE");
	CHECK(idx >= 0);
	CHECK(has(send_text((unsigned int)idx), "Route: <sip:rr.example.net;lr>"));
	CHECK(!has(send_text((unsigned int)idx), T_SR_TEXT));
	CHECK_UINT(count_hdr(send_text((unsigned int)idx), "Route:"), 1);

	/*
	 * The one next hop is the sticky registrar target and the outbound
	 * proxy at once (sip/reg.h), so a hop naming another host is written
	 * and never resolved. No code path reads a hop name.
	 */
	CASE("service route: a hop that names another host does not move the "
	     "next hop");
	fx_reset();
	fk.service_route = sr_take(T_SR_OTHER);
	CHECK(call_dial(0, 0, "1002", true));
	CHECK(has(send_text(0), "Route: <sip:far.example.org:5070;lr>\r\n"));
	CHECK(!strcmp(fk.send[0].host, T_PEER));
	CHECK_UINT(fk.send[0].port, T_PEER_PORT);
}

/* the text of the last NOTIFY that went out */
static const char *last_notify(void)
{
	int idx = find_send("NOTIFY");

	return idx < 0 ? "" : send_text((unsigned int)idx);
}

static void test_xfer_in_refusals(void)
{
	char big[512];
	char to[600];
	struct req_info ri;

	CASE("transferee: an out-of-dialog REFER meets the no-dialog tail, 481");
	fx_reset();
	rx("REFER sip:1001@" T_DOMAIN " SIP/2.0\r\n"
	   "Via: SIP/2.0/UDP " T_PEER ":5060;branch=z9hG4bK-refo\r\n"
	   "Max-Forwards: 70\r\n"
	   "From: <sip:1002@" T_DOMAIN ">;tag=peer-o\r\n"
	   "To: <sip:1001@" T_DOMAIN ">\r\n"
	   "Call-ID: refer-out-of-dialog\r\n"
	   "CSeq: 1 REFER\r\n"
	   "Refer-To: <sip:1003@" T_DOMAIN ">\r\n"
	   "Content-Length: 0\r\n"
	   "\r\n");
	CHECK(has(last_text(), "SIP/2.0 481"));

	CASE("transferee: a REFER during an outstanding exchange is 491");
	fx_reset();
	uac_up(NULL);
	CHECK(call_hold(0, 0));
	rx(refer_text(2, "<sip:1003@" T_DOMAIN ">", NULL));
	CHECK(has(last_text(), "SIP/2.0 491"));
	CHECK(find_send("INVITE sip:1003") < 0);

	CASE("transferee: a REFER on a leg that is clearing is refused too");
	fx_reset();
	uac_up(NULL);
	CHECK(call_hangup(0, 0));
	rx(refer_text(2, "<sip:1003@" T_DOMAIN ">", NULL));
	CHECK(has(last_text(), "SIP/2.0 491"));

	CASE("transferee: a second REFER while one is served is 486");
	refer_up("<sip:1003@" T_DOMAIN ">");
	CHECK(has(send_text(2), "SIP/2.0 202"));
	rx(refer_text(3, "<sip:1004@" T_DOMAIN ">", NULL));
	CHECK(has(last_text(), "SIP/2.0 486"));
	CHECK(find_send("INVITE sip:1004") < 0);

	/*
	 * The order of the intake is the contract: a leg that is already
	 * serving one referral refuses the next before it reads a word of its
	 * target, so the answer says why and not what is wrong with the URI.
	 */
	CASE("transferee: the second refusal comes before the target is read");
	refer_up("<sip:1003@" T_DOMAIN ">");
	rx(refer_text(3, "<http://example.net/x>", NULL));
	CHECK(has(last_text(), "SIP/2.0 486"));

	/*
	 * The pending one-exchange rule: this leg is already referring
	 * somebody else and the machine is waiting for the one outcome call.h
	 * promises on it. Serving an inbound referral would demote the leg that
	 * outcome has to be reported on, and PENDING{XFER} would strand.
	 */
	CASE("transferee: a leg with its own transfer outstanding refuses one, 491");
	fx_reset();
	uac_up(NULL);
	CHECK(call_xfer(0, 0, CALL_XFER_ATTENDED, "1004"));
	reply(202, NULL, NULL, NULL);
	rx(refer_text(3, "<sip:1003@" T_DOMAIN ">", NULL));
	CHECK(has(last_text(), "SIP/2.0 491"));
	CHECK(find_send("INVITE sip:1003") < 0);

	CASE("transferee: and its own outcome still reaches the machine");
	rx(xfer_notify(4, 2, "terminated;reason=noresource", "SIP/2.0 200 OK\r\n"));
	CHECK_UINT(ev.xfer_done, 1);
	CHECK(ev.xfer_ok);
	CHECK_UINT(ev.xfer_slot, 0);

	CASE("transferee: a REFER whose answer is still outstanding refuses one too");
	fx_reset();
	uac_up(NULL);
	CHECK(call_xfer(0, 0, CALL_XFER_BLIND, "1004"));
	rx(refer_text(3, "<sip:1003@" T_DOMAIN ">", NULL));
	CHECK(has(last_text(), "SIP/2.0 491"));

	/*
	 * The ghost-child arm: a leg that holds no UI name is the child of
	 * a referral before it takes one over and the leg it replaced once it
	 * has. A referral served there would place a child no name can reach.
	 */
	CASE("transferee: the replaced leg refuses a referral once the child holds the name");
	refer_up("<sip:1003@" T_DOMAIN ">");
	child_answer();
	rx(refer_text(3, "<sip:1004@" T_DOMAIN ">", NULL));
	CHECK(has(last_text(), "SIP/2.0 491"));
	CHECK(find_send("INVITE sip:1004") < 0);

	CASE("transferee: a REFER with no Refer-To at all is 400");
	fx_reset();
	uac_up(NULL);
	rx(uac_in_dialog("REFER", "peer-tag", 2, NULL, NULL));
	CHECK(has(last_text(), "SIP/2.0 400"));

	CASE("transferee: a Refer-To that is not a sip URI is 416");
	fx_reset();
	uac_up(NULL);
	rx(refer_text(2, "<http://example.net/x>", NULL));
	CHECK(has(last_text(), "SIP/2.0 416 Unsupported URI Scheme"));

	CASE("transferee: a tel: Refer-To is 416 for the same reason");
	fx_reset();
	uac_up(NULL);
	rx(refer_text(2, "<tel:+15551234>", NULL));
	CHECK(has(last_text(), "SIP/2.0 416"));

	CASE("transferee: a sip: Refer-To this side cannot read is 400");
	fx_reset();
	uac_up(NULL);
	rx(refer_text(2, "<sip:>", NULL));
	CHECK(has(last_text(), "SIP/2.0 400"));

	/* a cleartext INVITE toward a sips: target would break the scheme end
	 * to end, so the account transport decides */
	CASE("transferee: a sips: Refer-To on a cleartext account is 416");
	fx_reset();
	uac_up(NULL);
	rx(refer_text(2, "<sips:1003@" T_DOMAIN ">", NULL));
	CHECK(has(last_text(), "SIP/2.0 416 Unsupported URI Scheme"));
	CHECK(find_send("INVITE sips:") < 0);

	CASE("transferee: the same target on a TLS account is accepted");
	fx_reset();
	fk.transport = "TLS";
	uac_up(NULL);
	rx(refer_text(2, "<sips:1003@" T_DOMAIN ">", NULL));
	CHECK(has(send_text(fk.sends - 3), "SIP/2.0 202"));
	CHECK(child_invite(&ri));
	CHECK(!strcmp(ri.ruri, "sips:1003@" T_DOMAIN));
	fk.transport = NULL;

	CASE("transferee: a line with no free slot answers 486, the recorded limit");
	fx_reset();
	uac_up(NULL);
	CHECK(dial(0, 1, "1005"));
	rx(refer_text(2, "<sip:1003@" T_DOMAIN ">", NULL));
	CHECK(has(last_text(), "SIP/2.0 486"));
	CHECK(find_send("INVITE sip:1003") < 0);

	CASE("transferee: a Refer-To too wide for a request URI is 414");
	fx_reset();
	uac_up(NULL);
	memset(big, 'z', sizeof(big) - 1);
	big[sizeof(big) - 1] = 0;
	snprintf(to, sizeof(to), "<sip:%s@%s>", big, T_DOMAIN);
	rx(refer_text(2, to, NULL));
	CHECK(has(last_text(), "SIP/2.0 414 Request-URI Too Long"));
	CHECK(find_send("INVITE sip:zzz") < 0);

	/*
	 * A Replaces this side could not read still means an attended transfer.
	 * Completing it without the header would hand the subscriber's call
	 * away blind to the same target, so the referral is refused instead.
	 */
	CASE("transferee: an embedded Replaces with a broken escape is 400");
	fx_reset();
	uac_up(NULL);
	rx(refer_text(2, "<sip:1003@" T_DOMAIN "?Replaces=abc%G1>", NULL));
	CHECK(has(last_text(), "SIP/2.0 400"));
	CHECK(find_send("INVITE sip:1003") < 0);

	CASE("transferee: one that decodes to a CRLF is 400, not a header line");
	fx_reset();
	uac_up(NULL);
	rx(refer_text(2, "<sip:1003@" T_DOMAIN "?Replaces=a%0D%0AX%3A%20y>", NULL));
	CHECK(has(last_text(), "SIP/2.0 400"));
	CHECK(find_send("INVITE sip:1003") < 0);

	CASE("transferee: one that decodes to a NUL is 400 as well");
	fx_reset();
	uac_up(NULL);
	rx(refer_text(2, "<sip:1003@" T_DOMAIN "?Replaces=a%00b>", NULL));
	CHECK(has(last_text(), "SIP/2.0 400"));

	CASE("transferee: every refusal leaves the call it arrived on untouched");
	CHECK_UINT(ev.failed, 0);
	CHECK_UINT(ev.ended, 0);
	CHECK_UINT(med.stop_calls, 0);
	CHECK(slot_is(1));
}

static void test_xfer_in_accept(void)
{
	struct req_info ri;
	char ev_id[64];

	CASE("transferee: an in-dialog REFER is answered 202 Accepted");
	refer_up("<sip:1003@" T_DOMAIN ">");
	CHECK(has(send_text(2), "SIP/2.0 202 Accepted"));

	CASE("transferee: the 202 carries our Contact (RFC 3515 2.4)");
	CHECK(has(send_text(2), "Contact: <sip:1001@" T_HOST ":5060>\r\n"));

	CASE("transferee: the 202 carries the tag this dialog already used");
	CHECK(req_info_at(0, &ri));
	CHECK(has(send_text(2), ri.from_tag));

	/* RFC 3515 2.4.4: the id of the implicit subscription is the CSeq of
	 * the REFER, and 2.4.5 says the first NOTIFY is 100 Trying */
	CASE("transferee: the first NOTIFY names the subscription the REFER made");
	snprintf(ev_id, sizeof(ev_id), "Event: refer;id=%u\r\n", 2);
	CHECK(has(last_notify(), ev_id));

	CASE("transferee: it is active and carries the sipfrag of RFC 3515");
	CHECK(has(last_notify(), "Subscription-State: active\r\n"));
	CHECK(has(last_notify(), "Content-Type: message/sipfrag\r\n"));
	CHECK(has(last_notify(), "\r\n\r\nSIP/2.0 100 Trying\r\n"));

	CASE("transferee: the NOTIFY carries our Contact (RFC 6665 8.6)");
	CHECK(has(last_notify(), "Contact: <sip:1001@" T_HOST ":5060>\r\n"));

	CASE("transferee: no Refer-Sub is emitted (RFC 4488)");
	CHECK(!has(last_notify(), "Refer-Sub"));
	CHECK(!has(send_text(2), "Refer-Sub"));

	CASE("transferee: an ignored Refer-Sub on the REFER changes nothing");
	refer_up("<sip:1003@" T_DOMAIN ">");
	rx(refer_text(3, "<sip:1004@" T_DOMAIN ">", "Refer-Sub: false\r\n"));
	CHECK(has(last_text(), "SIP/2.0 486"));

	CASE("transferee: the child INVITE goes to the referred target");
	refer_up("<sip:1003@" T_DOMAIN ">");
	CHECK(child_invite(&ri));
	CHECK(!strcmp(ri.ruri, "sip:1003@" T_DOMAIN));
	CHECK(!strcmp(ri.to, "<sip:1003@" T_DOMAIN ">"));

	CASE("transferee: it is a call of its own, with its own Call-ID and tag");
	CHECK(req_info_at(0, &ri));
	{
		struct req_info child;

		CHECK(child_invite(&child));
		CHECK(strcmp(child.call_id, ri.call_id) != 0);
		CHECK(strcmp(child.from_tag, ri.from_tag) != 0);
	}

	/* the referral rides whatever dialog the leg has, and an answered call
	 * is the shape a transferor most often refers */
	CASE("transferee: a call this line ANSWERED takes a referral the same way");
	fx_reset();
	uas_up("in-refer");
	rx(uas_in_dialog("REFER", 2, "Refer-To: <sip:1003@" T_DOMAIN ">\r\n", NULL));
	CHECK(has(send_text(2), "SIP/2.0 202 Accepted"));
	CHECK(has(send_text(3), "Event: refer;id=2\r\n"));
	CHECK(child_invite_at() >= 0);
	CHECK(req_info_at((unsigned int)child_invite_at(), &ri));
	CHECK(!strcmp(ri.ruri, "sip:1003@" T_DOMAIN));

	/*
	 * The inbound arm: a leg this line answered carries no presentation
	 * verdict of its own, so the child keeps the line identity rather than
	 * reading the zero of the allocation as a decision.
	 */
	CASE("transferee: a child of an ANSWERED call keeps the line identity");
	CHECK(has(ri.from, "sip:1001@" T_DOMAIN));
	CHECK(!has(ri.from, "anonymous"));
	CHECK(!has(send_text((unsigned int)child_invite_at()), "Privacy: id"));

	CASE("transferee: the identity of the LINE is unchanged");
	refer_up("<sip:1003@" T_DOMAIN ">");
	CHECK(child_invite(&ri));
	CHECK(has(ri.from, "sip:1001@" T_DOMAIN));
	CHECK(!strcmp(ri.contact, "sip:1001@" T_HOST ":5060"));
	CHECK(!has(ri.from, "anonymous"));

	/* the outbound arm: that leg does carry the subscriber's own verdict,
	 * and the call the child places follows it either way */
	CASE("transferee: a withheld outbound parent makes the child withhold too");
	fx_reset();
	dial_present(0, 0, "1002", false);
	reply(100, NULL, NULL, NULL);
	reply(200, "peer-tag", NULL, sdp_body(T_PEER, 20000, "0 101", NULL));
	rx(refer_text(2, "<sip:1003@" T_DOMAIN ">", NULL));
	CHECK(child_invite(&ri));
	CHECK(has(ri.from, "sip:anonymous@anonymous.invalid"));
	CHECK(has(send_text((unsigned int)child_invite_at()), "Privacy: id"));

	CASE("transferee: and a presented one keeps the line identity");
	refer_up("<sip:1003@" T_DOMAIN ">");
	CHECK(child_invite(&ri));
	CHECK(has(ri.from, "sip:1001@" T_DOMAIN));
	CHECK(!has(send_text((unsigned int)child_invite_at()), "Privacy: id"));

	CASE("transferee: it takes the OTHER physical slot and no UI name of its own");
	CHECK(!call_slot_free(0, NULL));
	CHECK_UINT(call_slot_stream(0, 0), 0);
	CHECK_UINT(call_slot_stream(0, 1), CALL_SLOTS);

	CASE("transferee: it is INTERNAL, so no offer was reported to the FSM");
	CHECK_UINT(ev.incoming, 0);
	CHECK_UINT(ev.proceeding, 0);
	CHECK_UINT(ev.answered, 0);
	CHECK_UINT(ev.failed, 0);

	/*
	 * A leg no command placed and no offer rang still owes a record, and
	 * this edge is the only announcement it ever makes.
	 */
	CASE("transferee: the child announces its own record, on its own slot");
	CHECK_UINT(ev.started, 1);
	CHECK_UINT(ev.started_line, 0);
	CHECK_UINT(ev.started_slot, 1);
	CHECK(!ev.started_inbound);

	CASE("transferee: it is announced only once the INVITE is on the wire");
	CHECK_UINT(ev.started_sends, fk.sends);

	CASE("transferee: and an ordinary call announces no such leg at all");
	fx_reset();
	uac_up(NULL);
	CHECK_UINT(ev.started, 0);
	rx(invite_text("plain-in", NULL, sdp_body(T_PEER, 20030, "0 101", NULL)));
	CHECK_UINT(ev.started, 0);
}

static void test_xfer_in_child(void)
{
	struct req_info ri;
	char wide[420];
	char by[520];

	CASE("transferee: the R-URI is the Refer-To with its URI headers stripped");
	refer_up("<sip:1003@" T_DOMAIN "?Replaces=cid1%3Bto-tag%3Dt1%3Bfrom-tag%3Df1>");
	CHECK(child_invite(&ri));
	CHECK(!strcmp(ri.ruri, "sip:1003@" T_DOMAIN));

	CASE("transferee: the embedded Replaces becomes EXACTLY ONE header, unescaped");
	CHECK(has(send_text(4), "\r\nReplaces: cid1;to-tag=t1;from-tag=f1\r\n"));
	CHECK_UINT(count_hdr(send_text(4), "Replaces:"), 1);

	CASE("transferee: and RFC 3891 4 puts Require: replaces beside it");
	CHECK(has(send_text(4), "\r\nRequire: replaces\r\n"));

	CASE("transferee: a referral with no Replaces requires nothing");
	refer_up("<sip:1003@" T_DOMAIN ">");
	CHECK(!has(send_text(4), "Replaces:"));
	CHECK(!has(send_text(4), "Require: replaces"));

	/* call.c dial_view_get: the child tests the target alone, so a refresh
	 * that failed inside the call does not refuse the transfer */
	CASE("transferee: the child INVITE still leaves with no live registration");
	fx_reset();
	uac_up(NULL);
	fk.registered = false;
	rx(refer_text(2, "<sip:1003@" T_DOMAIN ">", NULL));
	CHECK(has(send_text(2), "SIP/2.0 202"));
	CHECK(child_invite(&ri));
	CHECK(!strcmp(ri.ruri, "sip:1003@" T_DOMAIN));

	CASE("transferee: the FIRST Replaces of a doubled URI header is the one promoted");
	refer_up("<sip:1003@" T_DOMAIN "?Replaces=one&Replaces=two>");
	CHECK(has(send_text(4), "\r\nReplaces: one\r\n"));
	CHECK_UINT(count_hdr(send_text(4), "Replaces:"), 1);
	CHECK(!has(send_text(4), "Replaces: two"));

	CASE("transferee: Referred-By is copied verbatim (RFC 3892 5)");
	fx_reset();
	uac_up(NULL);
	rx(refer_text(2, "<sip:1003@" T_DOMAIN ">",
		      "Referred-By: \"Bob\" <sip:1002@" T_DOMAIN ">;pref=1\r\n"));
	CHECK(has(send_text(4), "\r\nReferred-By: \"Bob\" <sip:1002@" T_DOMAIN ">;pref=1\r\n"));

	CASE("transferee: a Referred-By nobody sent leaves the child without one");
	refer_up("<sip:1003@" T_DOMAIN ">");
	CHECK(!has(send_text(4), "Referred-By"));

	/*
	 * RFC 3892 makes the header informational, so a display name wider than
	 * this side's budget must not cost the subscriber the transfer: the
	 * header is dropped and the referral stands.
	 */
	CASE("transferee: a Referred-By too wide for the child is dropped, not refused");
	fx_reset();
	uac_up(NULL);
	memset(wide, 'w', sizeof(wide) - 1);
	wide[sizeof(wide) - 1] = 0;
	snprintf(by, sizeof(by), "Referred-By: \"%s\" <sip:1002@" T_DOMAIN ">\r\n", wide);
	rx(refer_text(2, "<sip:1003@" T_DOMAIN ">", by));
	CHECK(has(send_text(2), "SIP/2.0 202"));
	CHECK(child_invite(&ri));
	CHECK(!strcmp(ri.ruri, "sip:1003@" T_DOMAIN));
	CHECK(!has(send_text(4), "Referred-By"));

	/*
	 * The promotion resurrects nothing: hdr.c refuses the escapes that
	 * would decode to a control byte, so the refusal cases above are what
	 * keeps a header line out of this buffer. This one proves the safe
	 * escapes survive it whole instead of being dropped with them.
	 */
	CASE("transferee: a Replaces of escaped separators survives whole");
	refer_up("<sip:1003@" T_DOMAIN "?Replaces=a%3Db%26c%3Bto-tag%3Dt%3Bfrom-tag%3Df>");
	CHECK(has(send_text(4), "\r\nReplaces: a=b&c;to-tag=t;from-tag=f\r\n"));

	CASE("transferee: the credential retry builds the child again WITH its Replaces");
	refer_up("<sip:1003@" T_DOMAIN "?Replaces=cid1%3Bto-tag%3Dt1%3Bfrom-tag%3Df1>");
	reply(407, "target-tag",
	      "Proxy-Authenticate: Digest realm=\"" T_DOMAIN "\", nonce=\"" T_NONCE "\"\r\n",
	      NULL);
	CHECK(has(last_text(), "INVITE sip:1003@" T_DOMAIN));
	CHECK(has(last_text(), "Proxy-Authorization: Digest"));
	CHECK(has(last_text(), "\r\nReplaces: cid1;to-tag=t1;from-tag=f1\r\n"));
	CHECK(has(last_text(), "\r\nRequire: replaces\r\n"));
}

static void test_xfer_in_swap(void)
{
	struct req_info ri, child;
	unsigned int stops;
	int idx;

	CASE("transferee: the child's 200 stops the replaced stream BEFORE its own");
	/* refer_up() inline, because the child is built while the REFER is read
	 * and the device it must copy has to stand before that. */
	fx_reset();
	fk.bind_dev = "lan4";
	uac_up(NULL);
	memset(&ev, 0, sizeof(ev));
	rx(refer_text(2, "<sip:1003@" T_DOMAIN ">", NULL));
	med.nseq = 0;
	med.stop_calls = 0;
	med.config_calls = 0;
	child_answer();
	CHECK(med_seq_index(MED_STOP) >= 0);
	CHECK(med_seq_index(MED_CONFIG) >= 0);
	CHECK(med_seq_index(MED_STOP) < med_seq_index(MED_CONFIG));
	CHECK(med_seq_index(MED_STOP) < med_seq_index(MED_START));

	CASE("transferee: the stopped stream is the replaced leg's");
	CHECK_UINT(med.snapshot_stream, 0);

	CASE("transferee: and the started one is the child's");
	CHECK_UINT(med.last_stream, 1);

	CASE("transferee: the child takes the line's device");
	CHECK(bind_dev_is("lan4"));

	CASE("transferee: the answer of the child reaches the FSM as nothing");
	CHECK_UINT(ev.answered, 0);
	CHECK_UINT(ev.proceeding, 0);
	CHECK_UINT(ev.failed, 0);
	CHECK_UINT(ev.ended, 0);

	CASE("transferee: the referral is reported done with the target's 200");
	CHECK(has(last_notify(), "Subscription-State: terminated;reason=noresource\r\n"));
	CHECK(has(last_notify(), "\r\n\r\nSIP/2.0 200 OK\r\n"));

	CASE("transferee: the UI slot moved, so a hangup of it reaches the child");
	CHECK(child_invite(&child));
	CHECK(call_hangup(0, 0));
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.method, "BYE"));
	CHECK(!strcmp(ri.call_id, child.call_id));

	/*
	 * call.h CALL_XFER_BYE_MS: a transferor that never ends the leg its
	 * referral replaced would leave two legs on one line for good.
	 */
	CASE("transferee: the guard BYEs the replaced leg when the transferor does not");
	refer_up("<sip:1003@" T_DOMAIN ">");
	child_answer();
	CHECK_UINT(count_send("BYE"), 0);
	fake_advance(CALL_XFER_BYE_MS - 1);
	CHECK_UINT(count_send("BYE"), 0);
	fake_advance(2);
	CHECK_UINT(count_send("BYE"), 1);

	CASE("transferee: that BYE went to the transferor and not to the target");
	CHECK(req_of("BYE", &ri));
	CHECK(!strcmp(ri.ruri, "sip:1002@" T_PEER ":5060"));

	CASE("transferee: and it reached the FSM as nothing");
	CHECK_UINT(ev.failed, 0);
	CHECK_UINT(ev.remote_bye, 0);

	CASE("transferee: the record of the replaced leg says a transfer ended it");
	reply(200, NULL, NULL, NULL);
	fake_advance(6000);
	CHECK_UINT(ev.ends[0][0].count, 1);
	CHECK(ev.ends[0][0].transferred);

	/*
	 * The record is the leg's and the name is the UI's. After the swap the
	 * child wears UI slot 0 and still closes the record it opened on slot 1,
	 * or the two legs of one transfer would fight over one record.
	 */
	CASE("transferee: the child closes the record it opened, under its own slot");
	refer_up("<sip:1003@" T_DOMAIN ">");
	child_answer();
	CHECK_UINT(ev.started_slot, 1);
	CHECK(call_hangup(0, 0));
	CHECK(find_send("BYE") >= 0);
	reply_at((unsigned int)find_send("BYE"), 200, NULL, NULL, NULL);
	fake_advance(6000);
	CHECK_UINT(ev.ends[0][1].count, 1);
	CHECK(!ev.ends[0][1].transferred);

	CASE("transferee: and the leg it replaced closes the one on slot 0");
	reply_at((unsigned int)find_send("BYE"), 200, NULL, NULL, NULL);
	fake_advance(6000);
	CHECK_UINT(ev.ends[0][0].count, 1);
	CHECK(ev.ends[0][0].transferred);

	CASE("transferee: a transferor that BYEs first leaves the guard nothing to do");
	refer_up("<sip:1003@" T_DOMAIN ">");
	child_answer();
	stops = med.stop_calls;
	rx(xfer_parent_req("BYE", 3, NULL, NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));
	fake_advance(CALL_XFER_BYE_MS + 1000);
	CHECK_UINT(count_send("BYE"), 0);
	CHECK_UINT(med.stop_calls, stops);

	CASE("transferee: and the child is still up on the UI slot it took");
	CHECK_UINT(ev.failed, 0);
	CHECK_UINT(ev.remote_bye, 0);
	CHECK(child_invite(&child));
	CHECK(call_hangup(0, 0));
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.method, "BYE"));
	CHECK(!strcmp(ri.call_id, child.call_id));

	/*
	 * The machine still believes the UI slot is held, so the leg that took
	 * it over has to make the wire agree. That exchange is ours and reports
	 * nothing, exactly as a session refresh does.
	 */
	CASE("transferee: a HELD replaced leg makes the child re-offer the hold");

	fx_reset();
	uac_up(NULL);
	CHECK(call_hold(0, 0));
	reply(200, "peer-tag", NULL, sdp_body(T_PEER, 20000, "0 101", NULL));
	CHECK_UINT(ev.reinvite_done, 1);
	rx(refer_text(3, "<sip:1003@" T_DOMAIN ">", NULL));
	med.config_live = 0;
	child_answer();
	CHECK(child_invite(&child));
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.method, "INVITE"));
	CHECK(!strcmp(ri.call_id, child.call_id));
	CHECK(has(ri.body, "a=sendonly"));

	/*
	 * The child is held before it knows it. Its own local hold is still
	 * false while it programs the stream it just answered, and the hold it
	 * inherits is re-offered only afterwards, so a description read from
	 * sdp_local_hold() alone would put the room on the wire for the whole
	 * of that exchange, and for good if the re-offer could not be sent. The
	 * xfer_held latch is what covers the gap, so the first media the child
	 * ever programs is already silent.
	 */
	CASE("transferee: the promoted child is silent from its first media on");
	CHECK(med.last.tx_silence);
	CHECK_UINT(med.config_live, 0);

	CASE("transferee: and that re-INVITE reports no operation to the FSM");
	reply(200, "target-tag", NULL, sdp_body(T_PEER, 20002, "0 101", NULL));
	CHECK_UINT(ev.reinvite_done, 1);

	CASE("transferee: the settle re-offer leaves the child silent");
	CHECK(med.last.tx_silence);
	CHECK_UINT(med.config_live, 0);

	CASE("transferee: an unheld replaced leg makes no re-INVITE at all");
	refer_up("<sip:1003@" T_DOMAIN ">");
	child_answer();
	CHECK_UINT(count_send("INVITE"), 2);
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.method, "NOTIFY"));

	/*
	 * The rollback is defined: the swap already moved the UI slot, so a
	 * stream the backend refuses fails through the ordinary uac path with
	 * that slot mapped, and the machine clears the call the subscriber is
	 * on instead of holding a name with no media.
	 */
	CASE("transferee: a child stream the backend refuses clears the UI slot");
	refer_up("<sip:1003@" T_DOMAIN ">");
	med.config_ret = HAL_FATAL;
	child_answer();
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.slot, 0);
	CHECK_UINT(ev.cls, CALL_FAIL_CONGESTION);

	CASE("transferee: and the transferor is told the referral failed here");
	CHECK(has(last_notify(), "Subscription-State: terminated;reason=noresource\r\n"));
	CHECK(has(last_notify(), "\r\n\r\nSIP/2.0 503 Service Unavailable\r\n"));
	med.config_ret = HAL_OK;

	/*
	 * The referral dials the child from a parent nothing holds, so the
	 * stream of an 18x answer would carry the subscriber's live audio to
	 * the target before the target answers. The 200 and its swap are the
	 * first moment the two sides belong together.
	 */
	CASE("xfer: the child of a transfer starts no early stream");
	cfg.channel[0].early_media_enabled = true;
	refer_up("<sip:1003@" T_DOMAIN ">");
	med.config_calls = 0;
	idx = child_invite_at();
	CHECK(idx >= 0);
	if (idx >= 0)
		reply_at((unsigned int)idx, 183, "target-tag", NULL,
			 sdp_body(T_PEER, 20002, "0 101", NULL));
	CHECK_UINT(med.config_calls, 0);
	child_answer();
	CHECK_UINT(med.config_calls, 1);
	CHECK_UINT(med.last_stream, 1);
	CHECK(has(last_notify(), "\r\n\r\nSIP/2.0 200 OK\r\n"));
	cfg.channel[0].early_media_enabled = false;

	/*
	 * A reliable 183 carries the one answer of the exchange (RFC 3261
	 * 13.2.1), so the child takes it before the rule above is read and
	 * still starts nothing. The 200 behind it then carries no body.
	 */
	CASE("xfer: the child takes a reliable 183 answer and still starts no "
	     "early stream");
	cfg.channel[0].early_media_enabled = true;
	refer_up("<sip:1003@" T_DOMAIN ">");
	med.config_calls = 0;
	idx = child_invite_at();
	CHECK(idx >= 0);
	if (idx >= 0)
		reply_at((unsigned int)idx, 183, "target-tag",
			 "Require: 100rel\r\nRSeq: 10\r\n",
			 sdp_body(T_PEER, 20002, "0 101", NULL));
	CHECK_UINT(med.config_calls, 0);
	CHECK_UINT(count_send("PRACK"), 1);
	CHECK_UINT(tr.prack_answer_lines, 1);
	if (idx >= 0)
		reply_at((unsigned int)idx, 200, "target-tag", NULL, NULL);
	CHECK_UINT(med.config_calls, 1);
	CHECK_UINT(med.last_stream, 1);
	CHECK(has(last_notify(), "\r\n\r\nSIP/2.0 200 OK\r\n"));
	cfg.channel[0].early_media_enabled = false;
}

static void test_xfer_in_silence(void)
{
	struct req_info ri, child;

	/*
	 * The transferor BYEs the replaced leg the moment it has our 202,
	 * while the child is still ringing. A remote_bye reported from there
	 * would tear the line to IDLE and orphan the child.
	 */
	CASE("transferee: the replaced leg's BYE is answered, recorded and silent");
	refer_up("<sip:1003@" T_DOMAIN ">");
	rx(xfer_parent_req("BYE", 3, NULL, NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(ev.remote_bye, 0);
	CHECK_UINT(ev.failed, 0);
	CHECK_UINT(ev.ends[0][0].count, 1);

	CASE("transferee: and the child that now holds the name still relays no ringing");
	reply_at((unsigned int)child_invite_at(), 180, "target-tag", NULL, NULL);
	CHECK_UINT(ev.proceeding, 0);
	CHECK_UINT(ev.answered, 0);

	CASE("transferee: and the child survives it and still answers");
	child_answer();
	CHECK_UINT(ev.answered, 0);
	CHECK_UINT(ev.failed, 0);
	CHECK(child_invite(&child));
	CHECK(call_hangup(0, 0));
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.method, "BYE"));
	CHECK(!strcmp(ri.call_id, child.call_id));

	/*
	 * A session watchdog that fires on the replaced leg inside the guard
	 * window would report a failure for a UI slot the child is about to
	 * own and clear a call that is up.
	 */
	CASE("transferee: the replaced leg's session timers are cancelled at demotion");
	fx_reset();
	uac_up("Session-Expires: 90;refresher=uas\r\n");
	CHECK(timer_armed(SIP_TIMER_CALL_WATCHDOG));
	memset(&ev, 0, sizeof(ev));
	rx(refer_text(2, "<sip:1003@" T_DOMAIN ">", NULL));
	CHECK(!timer_armed(SIP_TIMER_CALL_WATCHDOG));
	CHECK(!timer_armed(SIP_TIMER_CALL_SESSION));

	/* the target ringing keeps the child in its setup, so nothing but the
	 * cancelled watchdog can decide the next minute */
	CASE("transferee: the target ringing reports no progress to the FSM");
	reply(180, "target-tag", NULL, NULL);
	CHECK_UINT(ev.proceeding, 0);

	CASE("transferee: and the watchdog deadline passes with nothing failed");
	fake_advance(65000);
	CHECK_UINT(ev.failed, 0);
	CHECK_UINT(ev.remote_bye, 0);
	CHECK_UINT(ev.ends[0][0].count, 0);

	CASE("transferee: the replaced leg's own re-INVITE reports nothing either");
	refer_up("<sip:1003@" T_DOMAIN ">");
	rx(xfer_parent_req("INVITE", 3, NULL,
			   sdp_body(T_PEER, 20000, "0 101", "a=sendonly\r\n")));
	CHECK(has(last_text(), "SIP/2.0 200"));
	rx(xfer_parent_req("ACK", 3, NULL, NULL));
	CHECK_UINT(ev.hold_changed, 0);
	CHECK_UINT(ev.reinvite_done, 0);

	/*
	 * A leg that says nothing takes no operation that owes an answer: the
	 * machine enters PENDING on a hold and would wait there for a report
	 * the invariant swallows. The refusal reaches it through line/line.c's
	 * deferred-failure path instead.
	 */
	CASE("transferee: the demoted leg refuses the two ops that owe an outcome");
	CHECK(!call_hold(0, 0));
	CHECK(!call_resume(0, 0));
	CHECK_UINT(ev.reinvite_done, 0);

	CASE("transferee: and it still takes the ones that owe none");
	CHECK(call_dtmf(0, 0, '5', 100, false));
	CHECK(call_flash_relay(0, 0));

	CASE("transferee: a network call waiting INFO on it is answered and silent");
	rx(xfer_parent_req("INFO", 4, "Content-Type: application/broadsoft\r\n",
			   "play tone CallWaitingTone1\r\nCalling-Number: 5551234\r\n"));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(ev.cw_net, 0);

	CASE("transferee: an out of band digit INFO on it is answered and silent");
	rx(xfer_parent_req("INFO", 5, "Content-Type: application/dtmf-relay\r\n",
			   "Signal=5\r\nDuration=160\r\n"));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(ev.digit_net, 0);

	/*
	 * The control leg. The same reports on an ordinary leg of the same line
	 * still reach the machine, so the silence above is the invariant and
	 * not a broken fixture.
	 */
	CASE("transferee: the control leg on the other line still emits all of them");
	fx_reset();
	fk.line = 1;
	route_remember_id("ctrl-call", 1);
	rx_direct(1, invite_text_to(T_USER2, "ctrl-call", NULL,
				    sdp_body(T_PEER, 20010, "0 101", NULL)));
	CHECK_UINT(ev.incoming, 1);
	CHECK(call_answer(1, 0));
	rx(uas_in_dialog("ACK", 1, NULL, NULL));
	rx(uas_in_dialog("INFO", 2, "Content-Type: application/broadsoft\r\n",
			 "play tone CallWaitingTone1\r\nCalling-Number: 5551234\r\n"));
	CHECK_UINT(ev.cw_net, 1);
	rx(uas_in_dialog("INFO", 3, "Content-Type: application/dtmf-relay\r\n",
			 "Signal=5\r\nDuration=160\r\n"));
	CHECK_UINT(ev.digit_net, 1);
	rx(uas_in_dialog("BYE", 4, NULL, NULL));
	CHECK_UINT(ev.remote_bye, 1);
	CHECK_UINT(ev.ends[1][0].count, 1);
}

static void test_xfer_in_failure(void)
{
	struct req_info ri;

	CASE("transferee: a child final reports the target's status and terminates");
	refer_up("<sip:1003@" T_DOMAIN ">");
	reply(486, "target-tag", NULL, NULL);
	CHECK(has(last_notify(), "Subscription-State: terminated;reason=noresource\r\n"));
	CHECK(has(last_notify(), "\r\n\r\nSIP/2.0 486 Busy Here\r\n"));

	CASE("transferee: the child leg is released and the FSM heard nothing");
	CHECK_UINT(ev.failed, 0);
	CHECK_UINT(ev.answered, 0);
	CHECK_UINT(ev.ends[0][1].count, 1);
	CHECK(!ev.ends[0][1].transferred);

	CASE("transferee: the replaced leg speaks for its UI slot again");
	rx(xfer_parent_req("BYE", 3, NULL, NULL));
	CHECK_UINT(ev.remote_bye, 1);
	CHECK_UINT(ev.slot, 0);

	CASE("transferee: its media never stopped, so there was nothing to resume");
	CHECK_UINT(med.start_calls, 1);

	CASE("transferee: and its session timers came back with it");
	fx_reset();
	uac_up("Session-Expires: 90;refresher=uas\r\n");
	rx(refer_text(2, "<sip:1003@" T_DOMAIN ">", NULL));
	CHECK(!timer_armed(SIP_TIMER_CALL_WATCHDOG));
	reply(486, "target-tag", NULL, NULL);
	CHECK(timer_armed(SIP_TIMER_CALL_WATCHDOG));

	CASE("transferee: the subscriber stays on the old call after a failed referral");
	CHECK(call_hold(0, 0));
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.method, "INVITE"));
	CHECK(!strcmp(ri.ruri, "sip:1002@" T_PEER ":5060"));

	CASE("transferee: a child that could not be placed at all is reported too");
	fx_reset();
	uac_up(NULL);
	fk.send_fail = true;
	rx(refer_text(2, "<sip:1003@" T_DOMAIN ">", NULL));
	fk.send_fail = false;
	CHECK_UINT(ev.failed, 0);
	CHECK(call_hold(0, 0));

	/*
	 * A leg a transfer demoted still holds its UI slot, and the child holds
	 * none, so a teardown that suppressed both would leave the machine in a
	 * call state with no leg behind it.
	 */
	CASE("transferee: a line teardown during a referral still tells the FSM");
	refer_up("<sip:1003@" T_DOMAIN ">");
	calls_line_teardown(0);
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.slot, 0);
	CHECK_UINT(ev.cls, CALL_FAIL_QUIET);

	CASE("transferee: the child goes at once and the replaced leg drains its BYE");
	CHECK_UINT(ev.ends[0][1].count, 1);
	CHECK_UINT(ev.ends[0][0].count, 0);
	CHECK(find_send("BYE") >= 0);
	reply_at((unsigned int)find_send("BYE"), 200, NULL, NULL, NULL);
	fake_advance(6000);
	CHECK_UINT(ev.ends[0][0].count, 1);
	CHECK_UINT(ev.failed, 1);

	CASE("transferee: a teardown after the swap tells it once too");
	refer_up("<sip:1003@" T_DOMAIN ">");
	child_answer();
	memset(&ev, 0, sizeof(ev));
	calls_line_teardown(0);
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.slot, 0);

	CASE("transferee: an on-hook during the referral takes both legs with it");
	refer_up("<sip:1003@" T_DOMAIN ">");
	CHECK(call_hangup(0, 0));
	CHECK(has(last_notify(), "\r\n\r\nSIP/2.0 487 Request Terminated\r\n"));
	CHECK(count_send("BYE") + count_send("CANCEL") >= 1);
	CHECK(call_slot_free(0, NULL));
}

/*
 * The two-name funnel (call.h, the two-name contract). A dial names a UI slot
 * and the leg it makes lands on a physical one, and the two come apart exactly
 * once: after a transfer, when the child of the referral wears a name that is
 * not the slot it sits on.
 */
/*
 * The lifetime rules around the invariant. The invariant says which leg can
 * speak to the FSM: the leg that owns the UI slot. These rules say what a leg
 * can still do after it stops speaking, and what the two call records need
 * from each other.
 */
static void test_xfer_lifetime(void)
{
	struct req_info ri, child;
	struct slot_view sv;
	unsigned int stops;

	/*
	 * A leg the transfer replaced holds no stream, so a transferor that
	 * moves its port on the replaced dialog must not have that stream
	 * programmed and started again behind the swap.
	 */
	CASE("lifetime: the replaced leg refuses a peer re-INVITE with 488");
	refer_up("<sip:1003@" T_DOMAIN ">");
	child_answer();
	stops = med.start_calls;
	rx(xfer_parent_req("INVITE", 3, NULL, sdp_body(T_PEER, 20033, "0 101", NULL)));
	CHECK(has(last_text(), "SIP/2.0 488"));
	CHECK_UINT(med.start_calls, stops);

	CASE("lifetime: and the session timers it lost stay cancelled");
	CHECK(!timer_armed(SIP_TIMER_CALL_WATCHDOG));
	CHECK(!timer_armed(SIP_TIMER_CALL_SESSION));

	/* the leg that still carries the call keeps every exchange working:
	 * ui_holds() is what tells the demoted parent from the replaced one */
	CASE("lifetime: the DEMOTED leg still answers a peer re-INVITE");
	refer_up("<sip:1003@" T_DOMAIN ">");
	rx(xfer_parent_req("INVITE", 3, NULL,
			   sdp_body(T_PEER, 20034, "0 101", "a=sendonly\r\n")));
	CHECK(has(last_text(), "SIP/2.0 200"));

	/*
	 * The second arm: the name can move between our 2xx and the peer's
	 * ACK, and the exchange then completes on a leg that no longer carries
	 * the call.
	 */
	CASE("lifetime: an ACK landing after the swap restarts no stream");
	rx(xfer_parent_req("ACK", 3, NULL, NULL));
	refer_up("<sip:1003@" T_DOMAIN ">");
	rx(xfer_parent_req("INVITE", 3, NULL,
			   sdp_body(T_PEER, 20035, "0 101", "a=sendonly\r\n")));
	CHECK(has(last_text(), "SIP/2.0 200"));
	child_answer();
	stops = med.start_calls;
	rx(xfer_parent_req("ACK", 3, NULL, NULL));
	CHECK_UINT(med.start_calls, stops);
	CHECK_UINT(ev.failed, 0);

	/*
	 * The blind arm is the common arm, and the leg it ends never reaches
	 * the swap that sets the key on the other arms. Its record has to say a
	 * referral took the call away all the same.
	 */
	CASE("lifetime: the blind arm's record carries the transfer key");
	refer_up("<sip:1003@" T_DOMAIN ">");
	rx(xfer_parent_req("BYE", 3, NULL, NULL));
	CHECK_UINT(ev.ends[0][0].count, 1);
	CHECK(ev.ends[0][0].transferred);

	CASE("lifetime: and it says so even though the child had not answered");
	CHECK_UINT(ev.answered, 0);
	CHECK(child_invite(&child));
	child_answer();
	CHECK_UINT(ev.failed, 0);

	/*
	 * The hold fact latches onto the child when the referral is accepted,
	 * because the leg it was read from is usually gone by the time the
	 * child answers.
	 */
	CASE("lifetime: a held parent that dies first still makes the child hold");
	fx_reset();
	uac_up(NULL);
	CHECK(call_hold(0, 0));
	reply(200, "peer-tag", NULL, sdp_body(T_PEER, 20000, "0 101", NULL));
	rx(refer_text(3, "<sip:1003@" T_DOMAIN ">", NULL));
	rx(xfer_parent_req("BYE", 4, NULL, NULL));
	child_answer();
	CHECK(child_invite(&child));
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.method, "INVITE"));
	CHECK(!strcmp(ri.call_id, child.call_id));
	CHECK(has(ri.body, "a=sendonly"));

	/*
	 * A child that cannot speak must never be handed the name, or the
	 * machine sits in a call with no leg behind it. The child answers
	 * without a usable body, so it is failing when the parent's BYE lands.
	 */
	CASE("lifetime: a dead child leaves the ending to the leg it replaced");
	refer_up("<sip:1003@" T_DOMAIN ">");
	reply_at((unsigned int)child_invite_at(), 200, "target-tag", NULL, NULL);
	CHECK_UINT(ev.failed, 0);
	rx(xfer_parent_req("BYE", 3, NULL, NULL));
	CHECK_UINT(ev.remote_bye, 1);
	CHECK_UINT(ev.slot, 0);

	CASE("lifetime: and the machine heard it exactly once");
	CHECK_UINT(ev.failed, 0);
	CHECK_UINT(ev.ends[0][0].count, 1);

	/*
	 * A proxy that guards the dialog challenges the NOTIFY too, and an
	 * unanswered challenge loses the outcome the transferor waits for.
	 */
	CASE("lifetime: a challenged transfer NOTIFY is resent with credentials");
	refer_up("<sip:1003@" T_DOMAIN ">");
	reply_at((unsigned int)find_send("NOTIFY"), 407, "peer-tag",
		 "Proxy-Authenticate: Digest realm=\"" T_DOMAIN "\", nonce=\"" T_NONCE "\"\r\n",
		 NULL);
	CHECK_UINT(count_send("NOTIFY"), 2);
	CHECK(has(last_notify(), "Proxy-Authorization: Digest"));
	CHECK(has(last_notify(), "\r\n\r\nSIP/2.0 100 Trying\r\n"));
	CHECK(has(last_notify(), "Subscription-State: active\r\n"));

	CASE("lifetime: and a second challenge is not answered again");
	reply_at((unsigned int)find_send("NOTIFY"), 407, "peer-tag",
		 "Proxy-Authenticate: Digest realm=\"" T_DOMAIN "\", nonce=\"" T_NONCE "\"\r\n",
		 NULL);
	CHECK_UINT(count_send("NOTIFY"), 2);

	CASE("lifetime: the final NOTIFY is the one rebuilt when IT is challenged");
	refer_up("<sip:1003@" T_DOMAIN ">");
	reply(486, "target-tag", NULL, NULL);
	reply_at((unsigned int)find_send("NOTIFY"), 401, "peer-tag",
		 "WWW-Authenticate: Digest realm=\"" T_DOMAIN "\", nonce=\"" T_NONCE "\"\r\n",
		 NULL);
	CHECK(has(last_notify(), "Authorization: Digest"));
	CHECK(has(last_notify(), "\r\n\r\nSIP/2.0 486 Busy Here\r\n"));
	CHECK(has(last_notify(), "terminated;reason=noresource"));

	/* the sip/ua.h contract: allow_events rides wherever allow rides */
	CASE("lifetime: every Allow this layer writes carries Allow-Events");
	fx_reset();
	uac_up(NULL);
	CHECK(has(send_text(0), "Allow-Events: message-summary, refer\r\n"));
	fx_reset();
	uas_up("allow-events");
	CHECK(has(send_text(0), "Allow-Events: message-summary, refer\r\n"));
	CHECK(has(send_text(1), "Allow-Events: message-summary, refer\r\n"));

	/*
	 * The referral is decided at the moment the child's own answer fails,
	 * not a BYE drain later, wherever the parent still lives.
	 */
	CASE("lifetime: a child 2xx this side cannot use reports the referral at once");
	refer_up("<sip:1003@" T_DOMAIN ">");
	reply_at((unsigned int)child_invite_at(), 200, "target-tag", NULL, NULL);
	CHECK(has(last_notify(), "terminated;reason=noresource"));
	CHECK(has(last_notify(), "\r\n\r\nSIP/2.0 488 Not Acceptable Here\r\n"));

	CASE("lifetime: and the leg it replaced speaks for its name again");
	CHECK(call_hold(0, 0));
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.method, "INVITE"));

	/* a value copied verbatim into a request of ours carries no control
	 * byte, which is the rule hdr.c applies to the other half */
	CASE("lifetime: a Referred-By with a control byte is dropped, not copied");
	fx_reset();
	uac_up(NULL);
	rx(refer_text(2, "<sip:1003@" T_DOMAIN ">",
		      "Referred-By: <sip:1002@" T_DOMAIN ">;x=a\001b\r\n"));
	CHECK(has(send_text(2), "SIP/2.0 202"));
	CHECK(child_invite(&ri));
	CHECK(!has(send_text((unsigned int)child_invite_at()), "Referred-By"));

	/*
	 * The intake bound is the request URI this side builds, so the widest
	 * target it can carry is CALL_URI_MAX - 1 and one byte more is
	 * refused. Both sides of that edge are pinned.
	 */
	CASE("lifetime: a target exactly at the bound is accepted");
	{
		/* the request line the child would carry, built by the suite so
		 * the bound is not the implementation answering its own
		 * question; req_info.ruri is narrower than a URI, so the pin
		 * reads the wire */
		char uri[400];
		char to[520];
		char want[560];
		size_t fill = 319 - strlen("sip:@" T_DOMAIN);

		memset(uri, 'u', fill);
		uri[fill] = 0;
		snprintf(to, sizeof(to), "<sip:%s@%s>", uri, T_DOMAIN);
		snprintf(want, sizeof(want), "INVITE sip:%s@%s SIP/2.0\r\n", uri, T_DOMAIN);
		fx_reset();
		uac_up(NULL);
		rx(refer_text(2, to, NULL));
		CHECK(has(send_text(2), "SIP/2.0 202"));
		CHECK(child_invite_at() >= 0);
		CHECK(has(send_text((unsigned int)child_invite_at()), want));

		CASE("lifetime: and one byte past it is 414");
		memset(uri, 'u', fill + 1);
		uri[fill + 1] = 0;
		snprintf(to, sizeof(to), "<sip:%s@%s>", uri, T_DOMAIN);
		fx_reset();
		uac_up(NULL);
		rx(refer_text(2, to, NULL));
		CHECK(has(last_text(), "SIP/2.0 414"));
		CHECK(child_invite_at() < 0);
	}

	/* a leg that says nothing takes no transfer of its own */
	CASE("lifetime: a demoted leg refuses call_xfer");
	refer_up("<sip:1003@" T_DOMAIN ">");
	CHECK(!call_xfer(0, 0, CALL_XFER_BLIND, "1009"));
	CHECK_UINT(ev.xfer_done, 0);

	/* the guard BYE gives the UI up, so the line admits again */
	CASE("lifetime: the guard BYE frees the replaced leg's UI occupancy");
	refer_up("<sip:1003@" T_DOMAIN ">");
	child_answer();
	CHECK(!call_slot_free(0, NULL));
	fake_advance(CALL_XFER_BYE_MS + 1);
	CHECK(call_slot_free(0, NULL));

	/*
	 * The guard is armed on a leg that is about to be released, and
	 * leg_alloc() memsets the slot it lives in. A timer left armed across
	 * that memset fires on storage the next call owns.
	 */
	CASE("lifetime: the guard is cancelled when the replaced leg is released");
	refer_up("<sip:1003@" T_DOMAIN ">");
	child_answer();
	stops = timer_count(SIP_TIMER_CALL_C);
	CHECK(stops >= 1);
	rx(xfer_parent_req("BYE", 3, NULL, NULL));
	CHECK_UINT(timer_count(SIP_TIMER_CALL_C), stops - 1);

	CASE("lifetime: so a whole guard window later nothing fires on it");
	fake_advance(CALL_XFER_BYE_MS * 4);
	CHECK_UINT(ev.failed, 0);
	CHECK_UINT(fake_timer_dropped, 0);

	/*
	 * A transfer's child announces its answer to nobody, so the record
	 * would otherwise date that answer at the leg's end and show a call
	 * with no talk time. The age crosses in the ended report instead.
	 */
	CASE("lifetime: the child's ending reports how long ago it was answered");
	refer_up("<sip:1003@" T_DOMAIN ">");
	child_answer();
	fake_advance(30000);
	CHECK(call_hangup(0, 0));
	CHECK(find_send("BYE") >= 0);
	reply_at((unsigned int)find_send("BYE"), 200, NULL, NULL, NULL);
	fake_advance(6000);
	CHECK_UINT(ev.ends[0][1].count, 1);

	CASE("lifetime: and that age is the talk time, not the whole leg");
	CHECK_UINT(ev.end.slot, 1);
	CHECK(ev.end.answered);
	CHECK(ev.end.answered_ago_ms >= 30000);
	CHECK(ev.end.answered_ago_ms <= 36000);

	CASE("lifetime: a leg that never answered reports no age at all");
	fx_reset();
	CHECK(dial(0, 0, "1002"));
	reply(486, "peer-tag", NULL, NULL);
	CHECK_UINT(ev.ended, 1);
	CHECK(!ev.end.answered);
	CHECK_UINT(ev.end.answered_ago_ms, 0);

	/*
	 * The dump names both halves of the two-name contract, and the leg
	 * that holds no name at all says so.
	 */
	CASE("lifetime: an untransferred leg dumps one name twice and no shadow");
	fx_reset();
	uac_up(NULL);
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(sv.found);
	CHECK(sv.has_ui_slot);
	CHECK_UINT(sv.ui_slot, 0);
	CHECK(!sv.has_shadow);
	CHECK(sv.ui_busy);
	CHECK(!strcmp(sv.state, "active"));

	CASE("lifetime: the child of a referral dumps as a leg holding no name");
	rx(refer_text(2, "<sip:1003@" T_DOMAIN ">", NULL));
	CHECK(slot_view_get(0, 1, &sv));
	CHECK(sv.has_shadow);
	CHECK(sv.shadow);
	CHECK_UINT(sv.ui_slot, 1);

	/* the key says "does not speak for its name", which the demoted leg
	 * also does not: it still carries the call, and the machine is told
	 * nothing about it until the referral is decided */
	CASE("lifetime: and the leg it will replace keeps its name and goes quiet");
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(sv.has_shadow);
	CHECK_UINT(sv.ui_slot, 0);
	CHECK(sv.ui_busy);

	CASE("lifetime: after the swap the child names the slot it took over");
	child_answer();
	CHECK(slot_view_get(0, 1, &sv));
	CHECK(!sv.has_shadow);
	CHECK_UINT(sv.ui_slot, 0);

	CASE("lifetime: and the leg it replaced is the shadow now");
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(sv.has_shadow);
	CHECK(sv.shadow);
}

static void test_xfer_two_names(void)
{
	struct req_info ri;
	unsigned int stream;

	CASE("funnel: an idle UI slot runs on no stream at all");
	fx_reset();
	CHECK_UINT(call_slot_stream(0, 0), CALL_SLOTS);
	CHECK_UINT(call_slot_stream(0, CALL_SLOTS), CALL_SLOTS);
	CHECK_UINT(call_slot_stream(CFG_CHANNEL_MAX, 0), CALL_SLOTS);

	CASE("funnel: an ordinary leg is its own name");
	uac_up(NULL);
	CHECK_UINT(call_slot_stream(0, 0), 0);
	CHECK(dial(0, 1, "1005"));
	CHECK_UINT(call_slot_stream(0, 1), 1);

	CASE("funnel: a dial on a UI slot another leg holds reports congestion");
	ev.failed = 0;
	CHECK(!call_dial(0, 1, "1006", true));
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.slot, 1);
	CHECK_UINT(ev.cls, CALL_FAIL_CONGESTION);

	CASE("funnel: an unknown line or slot is still refused in silence");
	ev.failed = 0;
	CHECK(!call_dial(CFG_CHANNEL_MAX, 0, "1006", true));
	CHECK(!call_dial(0, CALL_SLOTS, "1006", true));
	CHECK(!call_dial(0, 0, "", true));
	CHECK(!call_dial(0, 0, NULL, true));
	CHECK_UINT(ev.failed, 0);

	CASE("funnel: after a swap the UI slot runs on the child's stream");
	refer_up("<sip:1003@" T_DOMAIN ">");
	child_answer();
	CHECK_UINT(call_slot_stream(0, 0), 1);

	CASE("funnel: and a dial on the free name lands on the freed slot");
	rx(xfer_parent_req("BYE", 3, NULL, NULL));
	CHECK(call_dial(0, 1, "1005", true));
	stream = call_slot_stream(0, 1);
	CHECK_UINT(stream, 0);
	CHECK_UINT(call_slot_stream(0, 0), 1);

	CASE("funnel: the two legs then run their own streams");
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.method, "INVITE"));
	CHECK(!strcmp(ri.ruri, "sip:1005@" T_DOMAIN));

	/*
	 * leg_alloc() releases a draining predecessor, and that release is
	 * what hands a shadowing child the name it was waiting for. A dial that
	 * asked for the same name must find it gone, or two legs would own one
	 * name and the resolver would answer with whichever it walked into.
	 */
	CASE("funnel: a dial refuses the name the allocation itself handed away");
	fx_reset();
	uac_up(NULL);
	rx(refer_text(2, "<sip:1003@" T_DOMAIN ">", NULL));
	CHECK(child_invite_at() >= 0);
	/* the replaced leg dies of its own exchange: it gives the UI up but
	 * stays in use for its BYE drain, still shadowed by a live child */
	med.config_ret = HAL_FATAL;
	rx(xfer_parent_req("INVITE", 3, NULL, sdp_body(T_PEER, 20044, "0 101", NULL)));
	med.config_ret = HAL_OK;
	CHECK_UINT(ev.failed, 0);
	ev.failed = 0;
	CHECK(!call_dial(0, 0, "1006", true));
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.slot, 0);
	CHECK_UINT(ev.cls, CALL_FAIL_CONGESTION);

	CASE("funnel: and the child that took the name over is the only owner");
	CHECK_UINT(call_slot_stream(0, 0), 1);
	CHECK_UINT(call_slot_stream(0, 1), CALL_SLOTS);

	CASE("funnel: the same shape with the child placed, then the parent BYEd");
	fx_reset();
	uac_up(NULL);
	rx(refer_text(2, "<sip:1003@" T_DOMAIN ">", NULL));
	CHECK(child_invite_at() >= 0);
	rx(xfer_parent_req("BYE", 3, NULL, NULL));
	CHECK_UINT(call_slot_stream(0, 0), 1);
	ev.failed = 0;
	CHECK(!call_dial(0, 0, "1006", true));
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.slot, 0);
	CHECK_UINT(ev.cls, CALL_FAIL_CONGESTION);

	CASE("funnel: and exactly one leg still owns that name");
	CHECK_UINT(call_slot_stream(0, 0), 1);
	CHECK(call_hangup(0, 0));

	CASE("funnel: an offer taking the freed slot takes the free NAME with it");
	refer_up("<sip:1003@" T_DOMAIN ">");
	child_answer();
	rx(xfer_parent_req("BYE", 3, NULL, NULL));
	ev.incoming = 0;
	rx(invite_text("cw-after-xfer", NULL, sdp_body(T_PEER, 20020, "0 101", NULL)));
	CHECK_UINT(ev.incoming, 1);
	CHECK_UINT(ev.slot, 1);
	CHECK_UINT(call_slot_stream(0, 1), 0);
	CHECK_UINT(call_slot_stream(0, 0), 1);
}

/*
 * The allow lists. The three default lists move together with the handler.
 * The cases check each list whole and by literal, because an assertion
 * written through the macro would follow whatever the macro became.
 */
static void test_xfer_allow_lists(void)
{
	CASE("the served method set names REFER, and the whole list is pinned");
	CHECK(!strcmp(UA_ALLOW_DEFAULT,
		      "INVITE, ACK, CANCEL, BYE, OPTIONS, INFO, NOTIFY, REFER, "
		      "PRACK"));

	CASE("allow_events rides with it and names the refer package");
	CHECK(!strcmp(UA_ALLOW_EVENTS_DEFAULT, "message-summary, refer"));

	CASE("and Accept names the body that package carries");
	CHECK(!strcmp(UA_ACCEPT_DEFAULT,
		      "application/sdp, application/simple-message-summary, "
		      "message/sipfrag"));

	CASE("every request this layer builds advertises the Allow list with "
	     "REFER");
	fx_reset();
	uac_up(NULL);
	CHECK(has(send_text(0),
		  "Allow: INVITE, ACK, CANCEL, BYE, OPTIONS, INFO, NOTIFY, "
		  "REFER, PRACK\r\n"));

	CASE("and so does the 2xx of an inbound call");
	fx_reset();
	uas_up("allow-list");
	CHECK(has(send_text(1),
		  "Allow: INVITE, ACK, CANCEL, BYE, OPTIONS, INFO, NOTIFY, "
		  "REFER, PRACK\r\n"));
}

/* --------------------------------------- the target's INVITE with Replaces */

/*
 * Role 5 of RFC 3515 (RFC 3891): this line is the party an attended transfer
 * is being transferred to, and the transferee completes it with an INVITE that
 * carries the Replaces of the dialog this line already holds with the
 * transferor.
 *
 * The Replaces value is oriented at the receiver (RFC 3891 3), so the two
 * builders below put our tag in to-tag and the peer's in from-tag. A case that
 * wants the wrong orientation swaps them by hand, which is the whole point of
 * the pair.
 */
static bool replaces_of_uac(char *out, size_t olen, const char *peer_tag)
{
	struct req_info ri;

	if (!req_info_at(0, &ri))
		return false;

	snprintf(out, olen, "%s;to-tag=%s;from-tag=%s", ri.call_id, ri.from_tag, peer_tag);

	return true;
}

static bool replaces_of_uas(char *out, size_t olen)
{
	char call_id[160], peer_tag[64], our_tag[64];

	if (!uas_tags(call_id, sizeof(call_id), peer_tag, sizeof(peer_tag), our_tag,
		      sizeof(our_tag)))
		return false;

	snprintf(out, olen, "%s;to-tag=%s;from-tag=%s", call_id, our_tag, peer_tag);

	return true;
}

/* the INVITE the transferee sends: a call of its own, with the Replaces of the
 * dialog it takes over */
static const char *replaces_invite(const char *call_id, const char *replaces,
				   const char *extra, const char *body)
{
	char hdrs[1024];

	snprintf(hdrs, sizeof(hdrs), "Replaces: %s\r\n%s", replaces, extra ? extra : "");

	return invite_text(call_id, hdrs, body);
}

/* the index of the 2xx this side answered the Replaces INVITE with */
static int rep_200;

static const char *replaces_sdp(void)
{
	return sdp_body(T_PEER, 20040, "0 101", NULL);
}

/*
 * An answered outbound call, then the INVITE that completes an attended
 * transfer onto it. The callback recorder and the media counters are cleared
 * between the two, so every count a case makes afterwards is a count of what
 * the replacement did and not of the call it replaces.
 */
static void replaces_up(const char *extra)
{
	char rep[400];

	fx_reset();
	uac_up(NULL);
	memset(&ev, 0, sizeof(ev));
	med.start_calls = 0;
	med.stop_calls = 0;
	med.config_calls = 0;
	med.nseq = 0;
	CHECK(replaces_of_uac(rep, sizeof(rep), "peer-tag"));
	rx(replaces_invite("repl-1", rep, extra, replaces_sdp()));
	rep_200 = find_send("SIP/2.0 200");
}

/* an in-dialog request from the peer of the call the Replaces INVITE created */
static const char *replaces_req(const char *method, unsigned int cseq, const char *extra,
				const char *body)
{
	char call_id[160], peer_tag[64], our_tag[64];

	if (!resp_tags_at(rep_200, call_id, sizeof(call_id), peer_tag, sizeof(peer_tag),
			  our_tag, sizeof(our_tag)))
		return "";

	return dialog_req_text(method, cseq, call_id, peer_tag, our_tag, extra, body);
}

/* the transferee ACKs our 2xx, which is where the takeover happens */
static void replaces_ack(void)
{
	rx(replaces_req("ACK", 1, NULL, NULL));
}

/*
 * Fill the UA core's 2xx retransmission table (sip/ua.h, UA_ACCEPT_MAX). A 2xx
 * the core cannot take over is the one shape where leg_answer() answers true
 * for a leg it has already abandoned: the response reached the wire, nothing
 * will repeat it, and no ACK will ever be reported for it.
 */
static void accept_table_fill(void)
{
	char text[512];
	unsigned int i;

	for (i = 0; i < UA_ACCEPT_MAX; i++) {
		struct sip_msg *m;

		snprintf(text, sizeof(text),
			 "SIP/2.0 200 OK\r\n"
			 "Via: SIP/2.0/UDP %s:%u;branch=z9hG4bK-fill-%u\r\n"
			 "From: <sip:1002@%s>;tag=fill-f%u\r\n"
			 "To: <sip:1001@%s>;tag=fill-t%u\r\n"
			 "Call-ID: fill-%u\r\n"
			 "CSeq: 1 INVITE\r\n"
			 "Content-Length: 0\r\n"
			 "\r\n",
			 T_PEER, T_PEER_PORT, i, T_DOMAIN, i, T_DOMAIN, i, i);

		m = msg_take(text);
		CHECK(m != NULL);
		if (m)
			CHECK(ua_accept_add(&ua, m, text, strlen(text), &fk.peer, 0));
	}
}

/* the transferor ends the dialog the replacement took over */
static const char *replaced_bye(unsigned int cseq)
{
	return xfer_parent_req("BYE", cseq, NULL, NULL);
}

/*
 * The admission reads the Replaces before it refuses for capacity. The cases
 * that check the order drive the INVITE straight into call.c, as the
 * admission cases of test_admission do. Reason: the request intake of
 * account.c answers a full line 486 by itself.
 */
static void test_replaces_admission(void)
{
	char rep[400];

	CASE("target: a line already in a call takes the completion, no ring");
	replaces_up(NULL);
	CHECK(rep_200 >= 0);
	CHECK(has(send_text(2), "SIP/2.0 200"));
	CHECK_UINT(count_send("SIP/2.0 180"), 0);

	CASE("target: and the subscriber was never offered it (the CW policy bypass)");
	CHECK_UINT(ev.incoming, 0);
	CHECK_UINT(ev.proceeding, 0);
	CHECK_UINT(ev.answered, 0);
	CHECK_UINT(ev.failed, 0);

	CASE("target: the 2xx carries the answer of the new leg");
	CHECK(has(send_text(2), "Content-Type: application/sdp"));
	CHECK(has(send_text(2), "m=audio 10002"));

	/*
	 * The order, pinned where it is observable. On a full line the capacity
	 * refusal comes behind the match, so it answers only what the Replaces
	 * itself did not already decide.
	 */
	CASE("target: a full line answers 481 for a Replaces that matches nothing");
	fx_reset();
	uac_up(NULL);
	CHECK(dial(0, 1, "1005"));
	CHECK(!call_slot_free(0, NULL));
	rx_direct(0, replaces_invite("repl-nomatch", "no-such-call;to-tag=a;from-tag=b",
				     NULL, replaces_sdp()));
	CHECK(has(last_text(), "SIP/2.0 481"));

	CASE("target: and 400 for one it cannot read, on the same full line");
	rx_direct(0, replaces_invite("repl-bad", "no-tags-at-all", NULL, replaces_sdp()));
	CHECK(has(last_text(), "SIP/2.0 400"));

	CASE("target: a Replaces with only one of the two tags is 400 as well");
	rx_direct(0, replaces_invite("repl-half", "cid;to-tag=a", NULL, replaces_sdp()));
	CHECK(has(last_text(), "SIP/2.0 400"));

	/* RFC 3891 3: more than one Replaces in one INVITE is a 400 */
	CASE("target: two Replaces headers are 400, never the first of them");
	CHECK(replaces_of_uac(rep, sizeof(rep), "peer-tag"));
	{
		char extra[440];

		snprintf(extra, sizeof(extra), "Replaces: %s\r\n", rep);
		rx_direct(0, replaces_invite("repl-two", rep, extra, replaces_sdp()));
	}
	CHECK(has(last_text(), "SIP/2.0 400"));

	CASE("target: the capacity 486 fires only once the match itself succeeded");
	rx_direct(0, replaces_invite("repl-full", rep, NULL, replaces_sdp()));
	CHECK(has(last_text(), "SIP/2.0 486"));

	CASE("target: every refusal above left both calls of that line alone");
	CHECK_UINT(ev.failed, 0);
	CHECK_UINT(ev.ended, 0);
	CHECK_UINT(med.stop_calls, 0);
	CHECK(!call_slot_free(0, NULL));

	/* an INVITE with no Replaces reads no Replaces and takes the
	 * admission of every inbound call */
	CASE("target: an INVITE without Replaces keeps the admission it always had");
	fx_reset();
	uac_up(NULL);
	CHECK(dial(0, 1, "1005"));
	rx_direct(0, invite_text("plain-full", NULL, replaces_sdp()));
	CHECK(has(last_text(), "SIP/2.0 486"));
	CHECK_UINT(ev.incoming, 0);

	CASE("target: and on a line with room it still rings");
	fx_reset();
	uac_up(NULL);
	rx(invite_text("plain-room", NULL, replaces_sdp()));
	CHECK_UINT(ev.incoming, 1);
	CHECK_UINT(ev.slot, 1);

	/*
	 * The two refusals in front of the Replaces stay in front of it: neither
	 * is a fact about the request, and a line whose DSP cannot carry a call
	 * cannot carry a replaced one either.
	 */
	CASE("target: an account going away answers 480, not the Replaces answer");
	fx_reset();
	uac_up(NULL);
	CHECK(replaces_of_uac(rep, sizeof(rep), "peer-tag"));
	fk.active = false;
	rx_direct(0, replaces_invite("repl-480", rep, NULL, replaces_sdp()));
	CHECK(has(last_text(), "SIP/2.0 480"));
	fk.active = true;

	CASE("target: a DSP that is not ready answers 503, not the Replaces answer");
	fx_reset();
	uac_up(NULL);
	CHECK(replaces_of_uac(rep, sizeof(rep), "peer-tag"));
	fk.hal_state = UVOICE_HAL_STARTING;
	rx_direct(0, replaces_invite("repl-503", rep, NULL, replaces_sdp()));
	CHECK(has(last_text(), "SIP/2.0 503"));
	fk.hal_state = UVOICE_HAL_READY;

	/*
	 * A replacement completes a transfer onto a call of this line, as the
	 * referral child does, so a failed refresh does not refuse it. Both go
	 * through the account intake.
	 */
	CASE("target: a replacement for a confirmed dialog is admitted with no live registration");
	fx_reset();
	uac_up(NULL);
	CHECK(replaces_of_uac(rep, sizeof(rep), "peer-tag"));
	fk.registered = false;
	rx(replaces_invite("repl-unreg", rep, NULL, replaces_sdp()));
	CHECK(find_send("SIP/2.0 200") >= 0);
	CHECK(find_send("SIP/2.0 480") < 0);

	CASE("target: a replacement that names no dialog answers 481 with no live registration");
	fx_reset();
	uac_up(NULL);
	fk.registered = false;
	rx(replaces_invite("repl-unreg-none", "no-such-call;to-tag=a;from-tag=b", NULL,
			   replaces_sdp()));
	CHECK(has(last_text(), "SIP/2.0 481"));

	/* the ghost ring is behind the match, because it is a fact about the
	 * offer and the leg that reads one exists by then */
	CASE("target: a replacement whose offer names no usable address is 488");
	fx_reset();
	uac_up(NULL);
	CHECK(replaces_of_uac(rep, sizeof(rep), "peer-tag"));
	rx(replaces_invite("repl-ghost", rep, NULL,
			   sdp_body("sip.example.com", 20040, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 488"));
	CHECK_UINT(ev.incoming, 0);

	CASE("target: and the call it named is still up and still speaking");
	CHECK(call_hold(0, 0));
	CHECK_UINT(call_slot_stream(0, 0), 0);
}

/*
 * The match (RFC 3891 3): the Call-ID, our local tag against to-tag and our
 * remote tag against from-tag, over the dialogs of one line.
 */
static void test_replaces_match(void)
{
	struct req_info ri;
	char rep[400];
	char swap[400];

	CASE("match: a Call-ID nobody holds is 481");
	fx_reset();
	uac_up(NULL);
	CHECK(req_info_at(0, &ri));
	snprintf(rep, sizeof(rep), "not-%s;to-tag=%s;from-tag=peer-tag", ri.call_id,
		 ri.from_tag);
	rx(replaces_invite("m-cid", rep, NULL, replaces_sdp()));
	CHECK(has(last_text(), "SIP/2.0 481"));

	/*
	 * The orientation, on a dialog this line created. to-tag is compared
	 * with our tag; a transferor that wrote its own there names no dialog
	 * here, and answering it would replace a call on the strength of two
	 * tags that belong to the wrong ends.
	 */
	CASE("match: the tags swapped on an outbound dialog are 481");
	snprintf(swap, sizeof(swap), "%s;to-tag=peer-tag;from-tag=%s", ri.call_id,
		 ri.from_tag);
	rx(replaces_invite("m-swap-uac", swap, NULL, replaces_sdp()));
	CHECK(has(last_text(), "SIP/2.0 481"));

	CASE("match: and the right way round on the same dialog is answered");
	CHECK(replaces_of_uac(rep, sizeof(rep), "peer-tag"));
	rx(replaces_invite("m-ok-uac", rep, NULL, replaces_sdp()));
	CHECK(has(last_text(), "SIP/2.0 200"));

	/* the same pair on a dialog the peer created, where our tag is the To
	 * tag of our own 2xx and the peer's is the From tag of its INVITE */
	CASE("match: the tags swapped on an inbound dialog are 481");
	fx_reset();
	uas_up("m-in");
	{
		char call_id[160], peer_tag[64], our_tag[64];

		CHECK(uas_tags(call_id, sizeof(call_id), peer_tag, sizeof(peer_tag), our_tag,
			       sizeof(our_tag)));
		snprintf(swap, sizeof(swap), "%s;to-tag=%s;from-tag=%s", call_id, peer_tag,
			 our_tag);
	}
	rx(replaces_invite("m-swap-uas", swap, NULL, replaces_sdp()));
	CHECK(has(last_text(), "SIP/2.0 481"));

	CASE("match: and the right way round on the inbound dialog is answered");
	CHECK(replaces_of_uas(rep, sizeof(rep)));
	rx(replaces_invite("m-ok-uas", rep, NULL, replaces_sdp()));
	CHECK(has(last_text(), "SIP/2.0 200"));

	/* this side replaces no dialog it has not accepted yet */
	CASE("match: an EARLY dialog is 486, the recorded descope");
	fx_reset();
	CHECK(dial(0, 0, "1002"));
	reply(100, NULL, NULL, NULL);
	reply(180, "peer-tag", NULL, NULL);
	CHECK(replaces_of_uac(rep, sizeof(rep), "peer-tag"));
	rx(replaces_invite("m-early", rep, NULL, replaces_sdp()));
	CHECK(has(last_text(), "SIP/2.0 486"));

	CASE("match: and the early call is untouched by the refusal");
	CHECK_UINT(ev.failed, 0);
	CHECK_UINT(ev.ended, 0);

	/* RFC 3891 7.1: early-only asks for an early dialog alone, and this side
	 * replaces none, so a confirmed match cannot honor it either */
	CASE("match: early-only on a CONFIRMED dialog is 486");
	fx_reset();
	uac_up(NULL);
	CHECK(replaces_of_uac(rep, sizeof(rep), "peer-tag"));
	{
		char eo[440];

		snprintf(eo, sizeof(eo), "%s;early-only", rep);
		rx(replaces_invite("m-eo", eo, NULL, replaces_sdp()));
	}
	CHECK(has(last_text(), "SIP/2.0 486"));
	CHECK_UINT(ev.ended, 0);

	CASE("match: the same value without the flag is answered");
	rx(replaces_invite("m-noeo", rep, NULL, replaces_sdp()));
	CHECK(has(last_text(), "SIP/2.0 200"));

	/* the lookup is per line: a dialog of the other line names nothing
	 * here */
	CASE("match: a dialog of the OTHER line is 481");
	fx_reset();
	uac_up(NULL);
	CHECK(replaces_of_uac(rep, sizeof(rep), "peer-tag"));
	rx_direct(1, replaces_invite("m-line", rep, NULL, replaces_sdp()));
	CHECK(has(last_text(), "SIP/2.0 481"));

	/* a dialog on its way out is the first window of the BYE race: the call
	 * the transferor meant is over */
	CASE("match: a leg whose own clearing is on the wire is 481");
	fx_reset();
	uac_up(NULL);
	CHECK(replaces_of_uac(rep, sizeof(rep), "peer-tag"));
	CHECK(call_hangup(0, 0));
	rx(replaces_invite("m-clearing", rep, NULL, replaces_sdp()));
	CHECK(has(last_text(), "SIP/2.0 481"));

	/* a leg another transfer has already claimed speaks for no name, and a
	 * second claim would leave two legs waiting for one */
	CASE("match: a leg a referral already demoted is 481");
	refer_up("<sip:1003@" T_DOMAIN ">");
	CHECK(replaces_of_uac(rep, sizeof(rep), "peer-tag"));
	rx_direct(0, replaces_invite("m-demoted", rep, NULL, replaces_sdp()));
	CHECK(has(last_text(), "SIP/2.0 481"));

	/*
	 * The clearing test on its own. A hangup gives the UI occupancy up with
	 * the leg, so the occupancy test above already covers it; the consult
	 * transfer is the one shape that keeps both of its legs busy while their
	 * BYEs drain (call.h call_xfer_consult), and there the clearing test is
	 * the whole of the refusal. The subscriber is on hook and the two calls
	 * are ending, so a replacement of either names a call that is over.
	 */
	/*
	 * The theft gate, one field at a time. The match is what stands between
	 * a stranger and the subscriber's call: an INVITE that guesses one of
	 * the three fields must not be enough, and neither a prefix nor a
	 * different case of a field is that field. Each case below gets every
	 * other field right, so nothing but the named one can be what refuses
	 * it.
	 */
	CASE("theft: a wrong to-tag alone is 481");
	fx_reset();
	uac_up(NULL);
	CHECK(req_info_at(0, &ri));
	snprintf(rep, sizeof(rep), "%s;to-tag=not-our-tag;from-tag=peer-tag", ri.call_id);
	rx(replaces_invite("t-to", rep, NULL, replaces_sdp()));
	CHECK(has(last_text(), "SIP/2.0 481"));

	CASE("theft: a wrong from-tag alone is 481");
	snprintf(rep, sizeof(rep), "%s;to-tag=%s;from-tag=not-peer-tag", ri.call_id,
		 ri.from_tag);
	rx(replaces_invite("t-from", rep, NULL, replaces_sdp()));
	CHECK(has(last_text(), "SIP/2.0 481"));

	CASE("theft: a to-tag that is a strict PREFIX of ours is 481");
	{
		char cut[64];

		CHECK(strlen(ri.from_tag) > 1);
		snprintf(cut, sizeof(cut), "%.*s", (int)strlen(ri.from_tag) - 1, ri.from_tag);
		snprintf(rep, sizeof(rep), "%s;to-tag=%s;from-tag=peer-tag", ri.call_id, cut);
		rx(replaces_invite("t-pfx", rep, NULL, replaces_sdp()));
	}
	CHECK(has(last_text(), "SIP/2.0 481"));

	CASE("theft: a Call-ID that is a strict PREFIX of ours is 481");
	{
		char cut[160];

		CHECK(strlen(ri.call_id) > 1);
		snprintf(cut, sizeof(cut), "%.*s", (int)strlen(ri.call_id) - 1, ri.call_id);
		snprintf(rep, sizeof(rep), "%s;to-tag=%s;from-tag=peer-tag", cut, ri.from_tag);
		rx(replaces_invite("t-cid", rep, NULL, replaces_sdp()));
	}
	CHECK(has(last_text(), "SIP/2.0 481"));

	/* RFC 3261 19.3: a tag is compared byte by byte, so a different case is
	 * a different tag. Both sides are pinned, because the two are read from
	 * opposite ends of the dialog. */
	CASE("theft: an upper-cased to-tag is 481");
	{
		char up[64];
		size_t i;

		snprintf(up, sizeof(up), "%s", ri.from_tag);
		for (i = 0; up[i]; i++)
			up[i] = (char)toupper((unsigned char)up[i]);
		/* the premise of the case: a tag with no letter could not differ */
		CHECK(strcmp(up, ri.from_tag) != 0);
		snprintf(rep, sizeof(rep), "%s;to-tag=%s;from-tag=peer-tag", ri.call_id, up);
		rx(replaces_invite("t-case", rep, NULL, replaces_sdp()));
	}
	CHECK(has(last_text(), "SIP/2.0 481"));

	CASE("theft: an upper-cased from-tag is 481 for the same reason");
	snprintf(rep, sizeof(rep), "%s;to-tag=%s;from-tag=PEER-TAG", ri.call_id, ri.from_tag);
	rx(replaces_invite("t-case2", rep, NULL, replaces_sdp()));
	CHECK(has(last_text(), "SIP/2.0 481"));

	CASE("theft: every one of them left the call alone, and the right value still works");
	CHECK_UINT(ev.failed, 0);
	CHECK_UINT(ev.ended, 0);
	CHECK(replaces_of_uac(rep, sizeof(rep), "peer-tag"));
	rx(replaces_invite("t-ok", rep, NULL, replaces_sdp()));
	CHECK(has(last_text(), "SIP/2.0 200"));

	/*
	 * The unterminated quote, end to end. The value carries both tags and
	 * would have matched; the unterminated quoted parameter hides the tail,
	 * so the request is answered 400 rather than replacing a dialog on half
	 * a value.
	 */
	CASE("match: a Replaces whose quoted parameter is never closed is 400");
	fx_reset();
	uac_up(NULL);
	CHECK(replaces_of_uac(rep, sizeof(rep), "peer-tag"));
	{
		char hostile[460];

		snprintf(hostile, sizeof(hostile), "%s;vendor=\"unterminated;early-only",
			 rep);
		rx(replaces_invite("m-quote", hostile, NULL, replaces_sdp()));
	}
	CHECK(has(last_text(), "SIP/2.0 400"));
	CHECK_UINT(ev.incoming, 0);

	CASE("match: and the same value with the quote CLOSED is read to its end");
	{
		char closed[460];

		snprintf(closed, sizeof(closed), "%s;vendor=\"a;b\";early-only", rep);
		rx(replaces_invite("m-quote-ok", closed, NULL, replaces_sdp()));
	}
	CHECK(has(last_text(), "SIP/2.0 486"));

	CASE("match: a leg clearing with its UI occupancy still held is 481");
	consult_up();
	CHECK(call_xfer_consult(0, 0, 1));
	reply(202, NULL, NULL, NULL);
	CHECK(!call_slot_free(0, NULL));
	CHECK(replaces_of_uac(rep, sizeof(rep), "peer-a"));
	rx_direct(0, replaces_invite("m-drain", rep, NULL, replaces_sdp()));
	CHECK(has(last_text(), "SIP/2.0 481"));
}

/*
 * The takeover. The 2xx answers without ringing, the ACK moves the name, and
 * the dialog the INVITE replaced is BYEd by this side (RFC 3891 4).
 */
static void test_replaces_takeover(void)
{
	struct req_info ri;
	unsigned int starts;

	CASE("takeover: the new leg holds no UI name until the ACK");
	replaces_up(NULL);
	CHECK_UINT(call_slot_stream(0, 0), 0);
	CHECK_UINT(call_slot_stream(0, 1), CALL_SLOTS);

	CASE("takeover: and it started no second stream on the line");
	CHECK_UINT(med.start_calls, 0);
	CHECK_UINT(med.stop_calls, 0);

	CASE("takeover: the record of the leg nobody announced is opened");
	CHECK_UINT(ev.started, 1);
	CHECK_UINT(ev.started_line, 0);
	CHECK_UINT(ev.started_slot, 1);
	CHECK(ev.started_inbound);

	CASE("takeover: and it names the leg whose call it took over");
	CHECK_UINT(ev.started_from, 0);

	CASE("takeover: the ACK stops the replaced stream before it starts its own");
	med.nseq = 0;
	replaces_ack();
	CHECK(med_seq_index(MED_STOP) >= 0);
	CHECK(med_seq_index(MED_CONFIG) >= 0);
	CHECK(med_seq_index(MED_STOP) < med_seq_index(MED_CONFIG));
	CHECK(med_seq_index(MED_STOP) < med_seq_index(MED_START));

	CASE("takeover: the stopped stream is the replaced leg's and the started one is ours");
	CHECK_UINT(med.snapshot_stream, 0);
	CHECK_UINT(med.last_stream, 1);

	CASE("takeover: the UI name moved onto the new leg");
	CHECK_UINT(call_slot_stream(0, 0), 1);
	CHECK_UINT(call_slot_stream(0, 1), CALL_SLOTS);

	CASE("takeover: and this side sent the BYE RFC 3891 4 puts on it");
	CHECK_UINT(count_send("BYE"), 1);
	CHECK(req_of("BYE", &ri));
	CHECK(!strcmp(ri.ruri, "sip:1002@" T_PEER ":5060"));

	/*
	 * A transferee arms CALL_XFER_BYE_MS on the leg it replaced, because
	 * the transferor owes that BYE and may never send it. Here this side
	 * owes it and has just sent it, so nothing is left to guard. The timer
	 * is counted by its object, the way the transferee battery counts it:
	 * the guard rides the timer id of the setup bound it inherits from.
	 */
	CASE("takeover: no guard is armed, because no other side still sends that BYE");
	CHECK_UINT(timer_count(SIP_TIMER_CALL_C), 0);

	CASE("takeover: the machine was told nothing at any point of the exchange");
	CHECK_UINT(ev.incoming, 0);
	CHECK_UINT(ev.proceeding, 0);
	CHECK_UINT(ev.answered, 0);
	CHECK_UINT(ev.failed, 0);
	CHECK_UINT(ev.remote_bye, 0);
	CHECK_UINT(ev.remote_cancel, 0);
	CHECK_UINT(ev.hold_changed, 0);
	CHECK_UINT(ev.reinvite_done, 0);
	CHECK_UINT(ev.xfer_done, 0);

	CASE("takeover: the replaced leg's record says a transfer ended it");
	CHECK(find_send("BYE") >= 0);
	reply_at((unsigned int)find_send("BYE"), 200, NULL, NULL, NULL);
	fake_advance(6000);
	CHECK_UINT(ev.ends[0][0].count, 1);
	CHECK(ev.ends[0][0].transferred);

	CASE("takeover: and the leg that took over closes its own record, on its own slot");
	CHECK(call_hangup(0, 0));
	CHECK(find_send("BYE") >= 0);
	reply_at((unsigned int)find_send("BYE"), 200, NULL, NULL, NULL);
	fake_advance(6000);
	CHECK_UINT(ev.ends[0][1].count, 1);
	CHECK(!ev.ends[0][1].transferred);

	CASE("takeover: a hangup of the name reaches the leg that wears it");
	replaces_up(NULL);
	replaces_ack();
	reply_at((unsigned int)find_send("BYE"), 200, NULL, NULL, NULL);
	CHECK(call_hangup(0, 0));
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.method, "BYE"));
	CHECK(!strcmp(ri.call_id, "repl-1"));

	CASE("takeover: the freed slot admits a new call again");
	replaces_up(NULL);
	CHECK(!call_slot_free(0, NULL));
	replaces_ack();
	CHECK(call_slot_free(0, NULL));

	CASE("takeover: an offerless replacement offers in the 2xx and commits at the ACK");
	{
		char rep[400];

		fx_reset();
		uac_up(NULL);
		memset(&ev, 0, sizeof(ev));
		CHECK(replaces_of_uac(rep, sizeof(rep), "peer-tag"));
		rx(replaces_invite("repl-offerless", rep, NULL, NULL));
		rep_200 = find_send("SIP/2.0 200");
		CHECK(rep_200 >= 0);
		CHECK(has(send_text((unsigned int)rep_200), "m=audio 10002"));
		starts = med.start_calls;
		rx(replaces_req("ACK", 1, NULL, sdp_body(T_PEER, 20042, "0 101", NULL)));
		CHECK_UINT(med.start_calls, starts + 1);
		CHECK_UINT(call_slot_stream(0, 0), 1);
	}

	/* the machine still believes the name is held, so the leg that took it
	 * over makes the wire agree, exactly as a transferee's child does */
	CASE("takeover: a HELD replaced leg makes the new leg re-offer the hold");
	{
		char rep[400];

		fx_reset();
		uac_up(NULL);
		CHECK(call_hold(0, 0));
		reply(200, "peer-tag", NULL, sdp_body(T_PEER, 20000, "0 101", NULL));
		CHECK_UINT(ev.reinvite_done, 1);
		memset(&ev, 0, sizeof(ev));
		CHECK(replaces_of_uac(rep, sizeof(rep), "peer-tag"));
		rx(replaces_invite("repl-held", rep, NULL, replaces_sdp()));
		rep_200 = find_send("SIP/2.0 200");
		replaces_ack();
		CHECK(last_req(&ri));
		CHECK(!strcmp(ri.method, "INVITE"));
		CHECK(!strcmp(ri.call_id, "repl-held"));
		CHECK(has(ri.body, "a=sendonly"));
	}

	CASE("takeover: and that re-INVITE reports no operation to the FSM");
	reply(200, "target-tag", NULL, sdp_body(T_PEER, 20044, "0 101", NULL));
	CHECK_UINT(ev.reinvite_done, 0);

	CASE("takeover: an unheld one makes no re-INVITE at all");
	replaces_up(NULL);
	replaces_ack();
	CHECK_UINT(count_send("INVITE"), 1);

	/*
	 * The swap already moved the name, so a stream the backend refuses fails
	 * through the ordinary path with that name mapped: the machine clears
	 * the call the subscriber is on rather than holding a name with no media.
	 */
	CASE("takeover: a stream the backend refuses clears the UI name");
	replaces_up(NULL);
	med.config_ret = HAL_FATAL;
	replaces_ack();
	med.config_ret = HAL_OK;
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.slot, 0);
	CHECK_UINT(ev.cls, CALL_FAIL_CONGESTION);

	/*
	 * The two shadow roles meet. A line that already carried a referral
	 * wears its names apart from its slots (call.h, the two-name contract),
	 * so the slot a replacement lands on is the one whose name the surviving
	 * leg is already wearing. The replacement takes no name at all, which is
	 * why the free-name walk of an offered call never runs for it, and the
	 * takeover then moves the one name it is owed onto it.
	 */
	CASE("takeover: a line that already carried a referral completes one too");
	refer_up("<sip:1003@" T_DOMAIN ">");
	child_answer();
	rx(xfer_parent_req("BYE", 3, NULL, NULL));
	CHECK_UINT(call_slot_stream(0, 0), 1);
	CHECK(call_slot_free(0, NULL));
	{
		struct req_info child;
		char rep[400];

		CHECK(child_invite(&child));
		snprintf(rep, sizeof(rep), "%s;to-tag=%s;from-tag=target-tag", child.call_id,
			 child.from_tag);
		memset(&ev, 0, sizeof(ev));
		rx(replaces_invite("repl-2nd", rep, NULL, replaces_sdp()));
		rep_200 = find_send("SIP/2.0 200");
		CHECK(rep_200 >= 0);
		CHECK(has(send_text((unsigned int)rep_200), "SIP/2.0 200"));
	}

	CASE("takeover: the replacement landed on the freed slot and holds no name");
	CHECK_UINT(call_slot_stream(0, 1), CALL_SLOTS);
	CHECK_UINT(ev.started_slot, 0);
	CHECK_UINT(ev.started_from, 1);

	CASE("takeover: and the ACK moves the name onto the slot it landed on");
	replaces_ack();
	CHECK_UINT(call_slot_stream(0, 0), 0);
	CHECK_UINT(ev.incoming, 0);
	CHECK_UINT(ev.failed, 0);

	/*
	 * The takeover is one-shot. This entry serves the ACK of every 2xx
	 * this side sends inside the dialog, so a peer re-INVITE behind the
	 * takeover would otherwise run the swap again: the replaced leg is
	 * still in use while its BYE drains, so its handle still resolves and
	 * it would be BYEd a second time.
	 */
	CASE("takeover: a peer re-INVITE behind it BYEs the replaced leg no second time");
	replaces_up(NULL);
	replaces_ack();
	CHECK_UINT(count_send("BYE"), 1);
	reply_at((unsigned int)find_send("BYE"), 200, NULL, NULL, NULL);
	rx(replaces_req("INVITE", 2, NULL, sdp_body(T_PEER, 20046, "0 101", NULL)));
	CHECK(has(last_text(), "SIP/2.0 200"));
	rx(replaces_req("ACK", 2, NULL, NULL));
	CHECK_UINT(count_send("BYE"), 1);
	CHECK_UINT(ev.failed, 0);

	CASE("takeover: and the hold is re-offered no second time either");
	{
		char rep[400];
		unsigned int offers;

		fx_reset();
		uac_up(NULL);
		CHECK(call_hold(0, 0));
		reply(200, "peer-tag", NULL, sdp_body(T_PEER, 20000, "0 101", NULL));
		memset(&ev, 0, sizeof(ev));
		CHECK(replaces_of_uac(rep, sizeof(rep), "peer-tag"));
		rx(replaces_invite("repl-once", rep, NULL, replaces_sdp()));
		rep_200 = find_send("SIP/2.0 200");
		replaces_ack();
		/* the takeover's own hold re-offer, answered so the leg is active */
		CHECK(last_req(&ri));
		CHECK(!strcmp(ri.method, "INVITE"));
		reply(200, "target-tag", NULL, sdp_body(T_PEER, 20044, "0 101", NULL));
		offers = count_send("INVITE");

		rx(replaces_req("INVITE", 2, NULL,
				sdp_body(T_PEER, 20048, "0 101", NULL)));
		CHECK(has(last_text(), "SIP/2.0 200"));
		rx(replaces_req("ACK", 2, NULL, NULL));
		CHECK_UINT(count_send("INVITE"), offers);
	}

	/*
	 * The discriminator, from the other role. A transferee's child wears
	 * the same shadow fields, and its own dialog takes re-INVITEs like any
	 * other: the ACK of one must complete no takeover and must not BYE the
	 * leg the child replaced, whose ending the transferor still owes and
	 * whose guard has not expired.
	 */
	CASE("takeover: a transferee child's re-INVITE ACK BYEs its parent no earlier");
	refer_up("<sip:1003@" T_DOMAIN ">");
	child_answer();
	CHECK_UINT(count_send("BYE"), 0);
	{
		struct req_info child;

		CHECK(child_invite(&child));
		rx(dialog_req_text("INVITE", 2, child.call_id, "target-tag", child.from_tag,
				   NULL, sdp_body(T_PEER, 20050, "0 101", NULL)));
		CHECK(has(last_text(), "SIP/2.0 200"));
		rx(dialog_req_text("ACK", 2, child.call_id, "target-tag", child.from_tag,
				   NULL, NULL));
	}
	CHECK_UINT(count_send("BYE"), 0);
	CHECK_UINT(ev.failed, 0);

	CASE("takeover: and the guard still ends it at its own deadline");
	fake_advance(CALL_XFER_BYE_MS + 1);
	CHECK_UINT(count_send("BYE"), 1);

	/*
	 * A replacement must survive its own answer. The UA core has no record
	 * left for the 2xx retransmission, so the leg is abandoned inside the
	 * answer that returned true. Linking it would demote the call the
	 * subscriber is on and hand its name to a corpse.
	 */
	CASE("takeover: a replacement the UA core cannot retransmit is not linked");
	{
		char rep[400];

		fx_reset();
		uac_up(NULL);
		memset(&ev, 0, sizeof(ev));
		accept_table_fill();
		CHECK(replaces_of_uac(rep, sizeof(rep), "peer-tag"));
		rx(replaces_invite("repl-nofit", rep, NULL, replaces_sdp()));
	}
	CHECK(find_send("SIP/2.0 200") >= 0);
	CHECK_UINT(ev.started, 0);

	CASE("takeover: the replaced call still owns its name and still speaks");
	CHECK_UINT(call_slot_stream(0, 0), 0);
	rx(replaced_bye(2));
	CHECK_UINT(ev.remote_bye, 1);
	CHECK_UINT(ev.slot, 0);

	/* the ACK that never comes: the replacement dies and the leg it was
	 * going to replace speaks for its name again */
	CASE("takeover: an ACK that never arrives leaves the original call speaking");
	replaces_up(NULL);
	fake_advance(64000);
	CHECK_UINT(ev.failed, 0);
	CHECK_UINT(ev.ends[0][1].count, 1);
	CHECK_UINT(call_slot_stream(0, 0), 0);

	CASE("takeover: and the far end of that original call is heard again");
	rx(replaced_bye(2));
	CHECK_UINT(ev.remote_bye, 1);
	CHECK_UINT(ev.slot, 0);
}

/*
 * The BYE race, both windows, and the arm taken for the second one (see the
 * comment beside replaces_takeover()).
 */
static void test_replaces_race(void)
{
	struct req_info ri;
	char rep[400];

	/*
	 * Window one, the dialog is gone before the 2xx. The leg it names is
	 * released, so nothing matches and the answer is the 481 of a dialog
	 * that does not exist.
	 */
	CASE("race: a replaced dialog the transferor already ended is 481");
	fx_reset();
	uac_up(NULL);
	CHECK(replaces_of_uac(rep, sizeof(rep), "peer-tag"));
	rx(replaced_bye(2));
	CHECK_UINT(ev.remote_bye, 1);
	rx(replaces_invite("race-gone", rep, NULL, replaces_sdp()));
	CHECK(has(last_text(), "SIP/2.0 481"));
	CHECK_UINT(ev.incoming, 0);

	/*
	 * Window two, the dialog dies between our 2xx and the ACK. The leg that
	 * replaces it is answered and alive, so its release hands the name over
	 * early rather than tearing the call down: the subscriber keeps talking,
	 * on the leg the transfer meant them to be on.
	 */
	CASE("race: a replaced dialog that dies after our 2xx is answered and silent");
	replaces_up(NULL);
	rx(replaced_bye(2));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(ev.remote_bye, 0);
	CHECK_UINT(ev.failed, 0);

	CASE("race: its record still closes as a transfer's");
	CHECK_UINT(ev.ends[0][0].count, 1);
	CHECK(ev.ends[0][0].transferred);

	CASE("race: and the name passed to the replacement at that release");
	CHECK_UINT(call_slot_stream(0, 0), 1);

	CASE("race: the ACK then completes on a leg that already holds the name");
	replaces_ack();
	CHECK_UINT(med.start_calls, 1);
	CHECK_UINT(call_slot_stream(0, 0), 1);
	CHECK_UINT(ev.failed, 0);

	CASE("race: and no BYE goes out for a dialog that is already gone");
	CHECK_UINT(count_send("BYE"), 0);

	CASE("race: the machine heard nothing across the whole window");
	CHECK_UINT(ev.incoming, 0);
	CHECK_UINT(ev.answered, 0);
	CHECK_UINT(ev.remote_bye, 0);
	CHECK_UINT(ev.failed, 0);

	CASE("race: the call the subscriber kept is the new one");
	CHECK(call_hangup(0, 0));
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.method, "BYE"));
	CHECK(!strcmp(ri.call_id, "repl-1"));

	/* the same window with the subscriber going on hook first: both legs go,
	 * and the one the FSM named is the last thing the frame releases */
	CASE("race: an on-hook between the 2xx and the ACK takes both legs");
	replaces_up(NULL);
	CHECK(call_hangup(0, 0));
	CHECK(call_slot_free(0, NULL));
	CHECK_UINT(ev.failed, 0);
}

/*
 * The option tag. The cases check it whole and by literal, as
 * test_xfer_allow_lists() checks the Allow lists. An assertion written through
 * the macro would follow whatever the macro became.
 */
static void test_replaces_supported(void)
{
	char rep[400];

	CASE("the option tag list names replaces, and the whole list is pinned");
	CHECK(!strcmp(UA_SUPPORTED_DEFAULT, "timer, replaces, 100rel"));

	CASE("every request this layer builds advertises the Supported list with "
	     "replaces");
	fx_reset();
	uac_up(NULL);
	CHECK(has(send_text(0), "Supported: timer, replaces, 100rel\r\n"));

	CASE("and so do the 180 and the 2xx of an inbound call");
	fx_reset();
	uas_up("sup-in");
	CHECK(has(send_text(0), "Supported: timer, replaces, 100rel\r\n"));
	CHECK(has(send_text(1), "Supported: timer, replaces, 100rel\r\n"));

	/*
	 * The replaces tag in the list is what admits the request that carries
	 * the header: RFC 3891 4 puts Require: replaces beside it, and the
	 * inbound policy answers 420 for every option tag the list does not name.
	 */
	CASE("an INVITE that REQUIRES replaces is admitted, not answered 420");
	fx_reset();
	uac_up(NULL);
	CHECK(replaces_of_uac(rep, sizeof(rep), "peer-tag"));
	rx(replaces_invite("sup-req", rep, "Require: replaces\r\n", replaces_sdp()));
	CHECK(find_send("SIP/2.0 420") < 0);
	CHECK(has(last_text(), "SIP/2.0 200"));

	CASE("and a tag the list does not name still answers 420");
	fx_reset();
	uac_up(NULL);
	rx(invite_text("sup-gruu", "Require: gruu\r\n", replaces_sdp()));
	CHECK(has(last_text(), "SIP/2.0 420"));
	CHECK(has(last_text(), "Unsupported: gruu"));
}

/* ------------------------------------------------ the G.711 fax upspeed */

/* the same call as uac_up(), answered with the codec the caller names */
static void uac_up_codec(const char *pts, const char *extra_sdp)
{
	call_dial(0, 0, "1002", true);
	reply(100, NULL, NULL, NULL);
	reply(180, "peer-tag", NULL, NULL);
	reply(200, "peer-tag", NULL, sdp_body(T_PEER, 20000, pts, extra_sdp));
}

/*
 * The fax switch. Two steps in one order: the DSP goes to passthrough first,
 * then the offer tells the far end what this side is already sending.
 * Everything below is a property of that pair.
 */
static void test_fax(void)
{
	struct slot_view sv;
	struct req_info ri;
	unsigned int before;
	unsigned int sends;

	fx_reset();
	uac_up_codec("18 101", NULL);

	CASE("fax: the leg runs on G.729 before the switch");
	CHECK_UINT(med.last.codec, HAL_CODEC_G729);
	before = fk.sends;

	CASE("fax: the switch is accepted and reaches the leg's own stream");
	CHECK(call_fax_start(0, 0));
	CHECK_UINT(med.fax_calls, 1);
	CHECK_UINT(med.fax_line, 0);
	CHECK_UINT(med.fax_stream, 0);

	CASE("fax: the profile names passthrough on the preferred G.711");
	CHECK_UINT(med.fax.mode, HAL_FAX_PASSTHROUGH);
	CHECK_UINT(med.fax.codec, HAL_CODEC_PCMU);
	CHECK_UINT(med.fax.codec_pt, 0);

	/* the manual route: this hardware clears both V.152 capability bits, so
	 * the flag has no reader and must stay at none */
	CASE("fax: and no V.152 modem or fax flag");
	CHECK_UINT(med.fax.modem_fax_flag, HAL_MODEM_FAX_NONE);

	CASE("fax: with the canceller, both suppressors, silence compression "
	     "and the adaptive jitter buffer off");
	CHECK(med.fax.ec_disable);
	CHECK(med.fax.vad_disable);
	CHECK(med.fax.cng_disable);
	CHECK(med.fax.silence_comp_disable);
	CHECK(med.fax.jb_fixed);

	/*
	 * The recorder took fk.sends when the profile arrived, so this
	 * compares the profile against the wire rather than against a second
	 * media op: the re-INVITE must not have left before the DSP was
	 * converted.
	 */
	CASE("fax: the profile was applied BEFORE the re-INVITE left");
	CHECK_UINT(med.fax_sends, before);
	CHECK_UINT(fk.sends, before + 1);
	CHECK(med_seq_index(MED_FAX) >= 0);

	CASE("fax: the upspeed is an INVITE on the next CSeq of the dialog");
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.method, "INVITE"));
	CHECK_UINT(ri.cseq, 2);

	CASE("fax: whose offer carries G.711 alone, telephone-event beside it");
	CHECK(has(ri.body, "RTP/AVP 0 8 101\r\n"));
	CHECK(has(ri.body, "a=rtpmap:0 PCMU/8000"));
	CHECK(has(ri.body, "a=rtpmap:8 PCMA/8000"));
	CHECK(!has(ri.body, "G729"));

	CASE("fax: and no comfort noise and no hold attribute");
	CHECK(!has(ri.body, "CN/8000"));
	CHECK(!has(ri.body, "a=sendonly"));

	CASE("fax: the answered exchange moves the stream to G.711");
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "0 101", NULL));
	CHECK_UINT(med.last.codec, HAL_CODEC_PCMU);
	CHECK_UINT(count_send("ACK"), 2);

	/*
	 * The upspeed re-programs the stream, and that programming has to carry
	 * the profile forward rather than revert it. Without the leg's fax state
	 * reaching media_fill(), this exact 200 puts the jitter buffer back
	 * to adaptive underneath a page that is already running.
	 */
	CASE("fax: the fixed jitter buffer survives the upspeed answer");
	CHECK_UINT(med.last.jb_mode, HAL_JB_FIXED);
	CHECK(!med.last.vad);
	CHECK(!med.last.cng);

	/*
	 * And so does the kind. The backend keeps the canceller off the carrier
	 * because this description says so: a profile record of the backend's
	 * own would have died with the description this very answer replaced.
	 */
	CASE("fax: and the answer carries the modem kind the profile armed");
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_MODEM);

	CASE("fax: and the negotiated buffer sizes ride through it unchanged");
	CHECK_UINT(med.last.jb_init_ms, 50);
	/* the default ceiling of the adaptive jitter buffer is 250 ms */
	CHECK_UINT(med.last.jb_max_ms, 250);

	/* the machine carries no fax state, so neither outcome names a cell */
	CASE("fax: and reports nothing to the machine");
	CHECK_UINT(ev.reinvite_done, 0);
	CHECK_UINT(ev.failed, 0);
	CHECK_UINT(ev.hold_changed, 0);

	/*
	 * The churn guard. The leg keeps the clamped list, so a peer
	 * that re-offers G.729 with G.711 behind it is answered with the G.711:
	 * a call that switched to a fax must not fall back into a codec the
	 * carrier cannot cross.
	 */
	CASE("fax: a later peer re-INVITE negotiates against G.711 alone");
	rx(uac_in_dialog("INVITE", "peer-tag", 1, NULL,
			 sdp_body(T_PEER, 20002, "18 0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(has(last_text(), "a=rtpmap:0 PCMU/8000"));
	CHECK(!has(last_text(), "G729"));

	/* any later programming, not only the upspeed's own: a peer exchange
	 * this side never asked for re-asserts the profile too */
	CASE("fax: and that exchange keeps the fixed jitter buffer as well");
	CHECK_UINT(med.last.jb_mode, HAL_JB_FIXED);

	CASE("fax: and the kind with it");
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_MODEM);

	/* --- the kind the V.21 preamble names --- */

	/* The peer's exchange closes first. A leg that still owes an ACK holds
	 * the kind for that answer instead of programming (call.h), and the
	 * cases below drive the programming half. */
	rx(uac_in_dialog("ACK", "peer-tag", 1, NULL, NULL));

	/*
	 * The second stage on the media side. The answer tone could not say
	 * whether a fax or a modem answered, so the profile armed the modem
	 * kind; the preamble says fax, and the backend runs a canceller for one
	 * and none for the other. The change travels on the description alone.
	 */
	CASE("fax: the V.21 preamble moves the leg to the fax kind");
	before = med.config_calls;
	sends = fk.sends;
	CHECK(call_fax_kind_set(0, 0, HAL_PASSTHROUGH_FAX));
	CHECK_UINT(med.config_calls, before + 1);
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_FAX);

	CASE("fax: with no offer on the wire and the same codec on the stream");
	CHECK_UINT(fk.sends, sends);
	CHECK_UINT(med.last.codec, HAL_CODEC_PCMU);
	CHECK_UINT(med.last.jb_mode, HAL_JB_FIXED);

	/* the preamble repeats through the T.30 negotiation, so the second one
	 * must not push the same description again */
	CASE("fax: a second preamble programs nothing");
	before = med.config_calls;
	CHECK(call_fax_kind_set(0, 0, HAL_PASSTHROUGH_FAX));
	CHECK_UINT(med.config_calls, before);

	CASE("fax: the status dump names the kind beside the passthrough");
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.fax, "passthrough"));
	CHECK(!strcmp(sv.fax_kind, "fax"));

	CASE("fax: a slot with no leg refuses the kind switch");
	CHECK(!call_fax_kind_set(0, 1, HAL_PASSTHROUGH_FAX));

	/* --- the preamble that arrives before the upspeed answer --- */

	/*
	 * The order the wire usually makes: the preamble follows the answer
	 * tone by a second or two, and the upspeed 200 OK has not landed yet.
	 * The committed answer is still the pre-fax one, so a program here
	 * would put G.729 back on a stream the profile already clamped to
	 * G.711. The leg holds the kind and the answer of the open exchange
	 * carries it.
	 */
	CASE("fax: a preamble under the open upspeed waits for the answer");
	fx_reset();
	uac_up_codec("18 101", NULL);
	CHECK(call_fax_start(0, 0));
	before = med.config_calls;
	CHECK(call_fax_kind_set(0, 0, HAL_PASSTHROUGH_FAX));
	CHECK_UINT(med.config_calls, before);

	CASE("fax: and the answer then programs the stream once, with the kind");
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "0 101", NULL));
	CHECK_UINT(med.config_calls, before + 1);
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_FAX);
	CHECK_UINT(med.last.codec, HAL_CODEC_PCMU);
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.fax_kind, "fax"));

	/*
	 * A refused program leaves the stream running the kind it already had,
	 * so the leg reports that one and not the one it wanted. The preamble
	 * repeats through the T.30 negotiation, and the next one tries again.
	 */
	CASE("fax: a refused kind program is retried by the next preamble");
	fx_reset();
	uac_up_codec("18 101", NULL);
	CHECK(call_fax_start(0, 0));
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "0 101", NULL));
	med.config_ret = HAL_FATAL;
	before = med.config_calls;
	CHECK(!call_fax_kind_set(0, 0, HAL_PASSTHROUGH_FAX));
	CHECK_UINT(med.config_calls, before + 1);
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.fax_kind, "modem"));

	CASE("fax: and the next one programs it once the stream takes it");
	med.config_ret = HAL_OK;
	CHECK(call_fax_kind_set(0, 0, HAL_PASSTHROUGH_FAX));
	CHECK_UINT(med.config_calls, before + 2);
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_FAX);
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.fax_kind, "fax"));

	/* --- a leg already committed to G.711 --- */

	CASE("fax: a leg already on G.711 keeps the committed codec");
	fx_reset();
	uac_up(NULL);
	CHECK_UINT(med.last.codec, HAL_CODEC_PCMU);
	CHECK(call_fax_start(0, 0));
	CHECK_UINT(med.fax.codec, HAL_CODEC_PCMU);
	CHECK_UINT(med.fax.codec_pt, 0);

	/* both references re-INVITE on every switch, and the offer withdraws
	 * more than the codec, so a leg that changes no codec still sends one */
	CASE("fax: and the upspeed still goes out for it");
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.method, "INVITE"));
	CHECK_UINT(ri.cseq, 2);
	CHECK(has(ri.body, "RTP/AVP 0 8 101\r\n"));

	CASE("fax: a second switch while the exchange runs is refused");
	CHECK(!call_fax_start(0, 0));
	CHECK_UINT(med.fax_calls, 1);

	/*
	 * The committed branch has to be the committed one. This channel
	 * prefers PCMU, which heads its offer list, and the peer chose PCMA. A
	 * switch that took the preferred G.711 instead of the committed one
	 * would hand the DSP a codec the far end is not sending, and the page
	 * would decode as noise while both ends believed they had agreed.
	 */
	CASE("fax: a PCMA-committed leg keeps PCMA on a PCMU-preferred channel");
	fx_reset();
	uac_up_codec("8 101", NULL);
	CHECK_UINT(med.last.codec, HAL_CODEC_PCMA);
	CHECK(call_fax_start(0, 0));
	CHECK_UINT(med.fax.codec, HAL_CODEC_PCMA);
	CHECK_UINT(med.fax.codec_pt, 8);

	/*
	 * The payload type is the channel's, never a constant. This one takes
	 * the fallback branch, on a channel that offers no PCMU at all, and its
	 * A-law payload type is configured: a profile carrying 0 would put the
	 * DSP on a payload type this leg never offered.
	 */
	CASE("fax: the configured g711a payload type reaches the profile");
	fx_reset();
	cfg.channel[0].g711a_ptype = 96;
	cfg.codecs = 2;
	cfg.codec[0].enabled = true;
	cfg.codec[0].tr104_id = 3;
	cfg.codec[1].enabled = true;
	cfg.codec[1].tr104_id = 2;
	uac_up_codec("18 101", NULL);
	CHECK_UINT(med.last.codec, HAL_CODEC_G729);
	CHECK(call_fax_start(0, 0));
	CHECK_UINT(med.fax.codec, HAL_CODEC_PCMA);
	CHECK_UINT(med.fax.codec_pt, 96);

	CASE("fax: and the clamped offer carries that same payload type");
	CHECK(last_req(&ri));
	CHECK(has(ri.body, "RTP/AVP 96 101\r\n"));
	CHECK(has(ri.body, "a=rtpmap:96 PCMA/8000"));
	cfg_default();

	/*
	 * The stream is the leg's own slot. A second leg on one line runs on
	 * stream 1, and a switch that named stream 0 would convert the call the
	 * subscriber is not on while leaving the fax leg in the voice path.
	 */
	CASE("fax: a leg on slot 1 converts stream 1, not stream 0");
	fx_reset();
	uas_up("call-fax-s0");
	rx(invite_text("call-fax-s1", NULL, sdp_body(T_PEER, 20002, "0 101", NULL)));
	CHECK_UINT(ev.slot, 1);
	CHECK(call_ring(0, 1));
	CHECK(call_answer(0, 1));
	rx(uas_in_dialog("ACK", 1, NULL, NULL));
	CHECK(call_fax_start(0, 1));
	CHECK_UINT(med.fax_calls, 1);
	CHECK_UINT(med.fax_line, 0);
	CHECK_UINT(med.fax_stream, 1);

	/* --- the far end refuses the upspeed --- */

	CASE("fax: a 488 to the upspeed keeps the call and reports nothing");
	fx_reset();
	uac_up_codec("18 101", NULL);
	CHECK(call_fax_start(0, 0));
	reply(488, NULL, NULL, NULL);
	CHECK_UINT(ev.failed, 0);
	CHECK_UINT(ev.reinvite_done, 0);
	CHECK_UINT(med.stop_calls, 0);
	CHECK(find_send("BYE") < 0);

	CASE("fax: the committed media survives the refused offer");
	CHECK_UINT(med.last.codec, HAL_CODEC_G729);

	CASE("fax: and the profile the DSP already took is never undone");
	CHECK_UINT(med.fax_calls, 1);
	CHECK_UINT(med.fax.mode, HAL_FAX_PASSTHROUGH);

	/*
	 * The clamp outlives the refusal. This side is in passthrough whatever
	 * the far end answered, so every later exchange still has to describe
	 * G.711: an offer that went back to G.729 would ask the peer to send a
	 * codec the DSP is no longer decoding as voice.
	 */
	CASE("fax: a later exchange after the 488 still offers G.711 alone");
	CHECK(call_hold(0, 0));
	CHECK(last_req(&ri));
	CHECK(has(ri.body, "RTP/AVP 0 8 101\r\n"));
	CHECK(!has(ri.body, "a=rtpmap:18 G729/8000"));
	CHECK(has(ri.body, "a=sendonly"));

	CASE("fax: and that exchange still programs the fax stream fields");
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "0", "a=recvonly\r\n"));
	CHECK_UINT(med.last.jb_mode, HAL_JB_FIXED);
	CHECK(!med.last.vad);
	CHECK(!med.last.cng);

	CASE("fax: a 5xx to the upspeed leaves the same call standing");
	fx_reset();
	uac_up(NULL);
	CHECK(call_fax_start(0, 0));
	reply(503, NULL, NULL, NULL);
	CHECK_UINT(ev.failed, 0);
	CHECK_UINT(med.stop_calls, 0);
	CHECK_UINT(med.fax_calls, 1);

	/* --- the suppressors the offer has to withdraw --- */

	CASE("fax: a call that ran with silence suppression and comfort noise");
	fx_reset();
	cfg.channel[0].cn_enable = true;
	cfg.codecs = 1;
	cfg.codec[0].enabled = true;
	cfg.codec[0].tr104_id = 1;
	cfg.codec[0].silence_suppression = true;
	uac_up_codec("0 13", "a=rtpmap:13 CN/8000\r\n");
	CHECK(med.last.vad);
	CHECK(med.last.cng);

	CASE("fax: offers neither on the upspeed");
	CHECK(call_fax_start(0, 0));
	CHECK(last_req(&ri));
	CHECK(!has(ri.body, "CN/8000"));

	/*
	 * A fax carrier through a VAD gate loses the quiet parts of the page,
	 * so the flag has to be gone from the description the stream runs on,
	 * not only from the bytes: for G.711 the offer carries no attribute
	 * that would say it.
	 */
	CASE("fax: and the answered stream carries neither");
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "0", NULL));
	CHECK(!med.last.vad);
	CHECK(!med.last.cng);
	CHECK_UINT(med.last.jb_mode, HAL_JB_FIXED);

	/*
	 * The flag dies with the leg. The next call on this slot is an
	 * ordinary voice call, and its stream has to be filled as one: a fax
	 * state that outlived its leg would hold a fixed buffer under every
	 * later call the subscriber makes.
	 */
	CASE("fax: the next call on the slot fills an adaptive buffer again");
	rx(uac_in_dialog("BYE", "peer-tag", 1, NULL, NULL));
	CHECK_UINT(ev.ended, 1);
	CHECK(dial(0, 0, "1002"));
	reply(100, NULL, NULL, NULL);
	reply(200, "peer-tag2", NULL,
	      sdp_body(T_PEER, 20004, "0 13", "a=rtpmap:13 CN/8000\r\n"));
	CHECK_UINT(med.last.jb_mode, HAL_JB_ADAPTIVE);
	CHECK(med.last.vad);
	CHECK(med.last.cng);
	cfg_default();

	/* --- the refusals --- */

	CASE("fax: a backend that refuses the profile sends no re-INVITE");
	fx_reset();
	uac_up_codec("18 101", NULL);
	before = fk.sends;
	med.fax_ret = HAL_UNSUPPORTED;
	CHECK(!call_fax_start(0, 0));
	CHECK_UINT(med.fax_calls, 1);
	CHECK_UINT(fk.sends, before);
	med.fax_ret = HAL_OK;

	/* the clamp rides the success of the profile: a leg that never went to
	 * passthrough keeps negotiating with the codec list it always had */
	CASE("fax: and the leg keeps its full codec list for the next exchange");
	CHECK_UINT(ev.failed, 0);
	CHECK(call_hold(0, 0));
	CHECK(last_req(&ri));
	CHECK(has(ri.body, "a=rtpmap:18 G729/8000"));
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "18", "a=recvonly\r\n"));

	/* the fax state rides the same success: a profile the backend refused
	 * leaves an ordinary voice leg, so its stream is filled as one */
	CASE("fax: a refused profile never marks the leg as running a fax");
	CHECK_UINT(med.last.jb_mode, HAL_JB_ADAPTIVE);
	CHECK(call_resume(0, 0));
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "18", NULL));

	CASE("fax: and a later switch still converts it");
	before = fk.sends;
	CHECK(call_fax_start(0, 0));
	CHECK_UINT(fk.sends, before + 1);
	CHECK(last_req(&ri));
	CHECK(has(ri.body, "RTP/AVP 0 8 101\r\n"));

	CASE("fax: a slot with no leg refuses the switch");
	fx_reset();
	CHECK(!call_fax_start(0, 0));
	CHECK_UINT(med.fax_calls, 0);

	CASE("fax: a leg that is not yet answered refuses it too");
	CHECK(dial(0, 0, "1002"));
	reply(180, "peer-tag", NULL, NULL);
	CHECK(!call_fax_start(0, 0));
	CHECK_UINT(med.fax_calls, 0);

	/* the kind describes a carrier, so a voice leg has nothing to name and
	 * its stream must not be programmed for one */
	CASE("fax: the kind switch on a leg without a fax is refused");
	fx_reset();
	uac_up(NULL);
	before = med.config_calls;
	CHECK(!call_fax_kind_set(0, 0, HAL_PASSTHROUGH_FAX));
	CHECK_UINT(med.config_calls, before);
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_OFF);
}

/*
 * The challenged upspeed of a field capture. The switch answers the
 * G.711 re-INVITE 401, and the unit repeats the same offer with credentials
 * 62 ms later. The DSP took the profile before the first offer left. Every
 * retry here therefore describes a carrier that the stream already runs, and
 * a 491 hands the offer to the backoff instead.
 */
static void test_fax_auth(void)
{
	const char *chal = "WWW-Authenticate: Digest realm=\"" T_DOMAIN
			   "\", nonce=\"" T_NONCE "\"\r\n";
	const char *chal2 = "WWW-Authenticate: Digest realm=\"" T_DOMAIN
			    "\", nonce=\"a-second-nonce\"\r\n";
	const char *pchal = "Proxy-Authenticate: Digest realm=\"" T_DOMAIN
			    "\", nonce=\"" T_NONCE "\"\r\n";
	struct req_info first, retry;
	struct slot_view sv;
	unsigned int sends;

	CASE("fax auth: a 401 to the upspeed is acknowledged and answered "
	     "with one retry");
	fx_reset();
	uac_up_codec("18 101", NULL);
	CHECK(call_fax_start(0, 0));
	CHECK(req_of("INVITE", &first));
	sends = count_send("INVITE");
	reply(401, NULL, chal, NULL);
	CHECK_UINT(count_send("ACK"), 2);
	CHECK_UINT(count_send("INVITE"), sends + 1);

	CASE("fax auth: the credentialed retry still offers G.711 alone");
	CHECK(last_req(&retry));
	CHECK(has(last_text(), "Authorization: Digest"));
	CHECK(has(last_text(), "nonce=\"" T_NONCE "\""));
	CHECK(has(retry.body, "RTP/AVP 0 8 101\r\n"));
	CHECK(has(retry.body, "a=rtpmap:0 PCMU/8000"));
	CHECK(has(retry.body, "a=rtpmap:8 PCMA/8000"));
	CHECK(!has(retry.body, "a=rtpmap:18"));

	CASE("fax auth: it keeps the dialog and moves the CSeq");
	CHECK(!strcmp(retry.call_id, first.call_id));
	CHECK(!strcmp(retry.from_tag, first.from_tag));
	CHECK_UINT(retry.cseq, first.cseq + 1);

	CASE("fax auth: the profile the DSP already took is not applied a "
	     "second time");
	CHECK_UINT(med.fax_calls, 1);
	CHECK_UINT(med.fax.codec, HAL_CODEC_PCMU);
	CHECK_UINT(med.fax.codec_pt, 0);

	CASE("fax auth: the answered retry moves the stream to G.711 with the "
	     "kind");
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "0 101", NULL));
	CHECK_UINT(med.last.codec, HAL_CODEC_PCMU);
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_MODEM);
	CHECK_UINT(med.last.jb_mode, HAL_JB_FIXED);
	CHECK_UINT(med.fax_calls, 1);
	CHECK_UINT(ev.reinvite_done, 0);

	CASE("fax auth: a proxy challenge to the upspeed is its own exchange");
	fx_reset();
	uac_up_codec("18 101", NULL);
	CHECK(call_fax_start(0, 0));
	sends = count_send("INVITE");
	reply(407, NULL, pchal, NULL);
	CHECK_UINT(count_send("INVITE"), sends + 1);
	CHECK(has(last_text(), "Proxy-Authorization: Digest"));
	CHECK(last_req(&retry));
	CHECK(has(retry.body, "RTP/AVP 0 8 101\r\n"));

	CASE("fax auth: a second challenge of the same kind gives up");
	fx_reset();
	uac_up_codec("18 101", NULL);
	CHECK(call_fax_start(0, 0));
	reply(401, NULL, chal, NULL);
	sends = count_send("INVITE");
	reply(401, NULL, chal2, NULL);
	CHECK_UINT(count_send("INVITE"), sends);

	/*
	 * reinvite_done() puts c->op back to CALL_OP_NONE and reports no fax
	 * outcome at all, so the counts below and the operation the leg then
	 * takes are what the cleared field looks like from outside.
	 */
	CASE("fax auth: and the call, the committed media and the applied "
	     "profile stand");
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.state, "active"));
	CHECK(!strcmp(sv.fax, "passthrough"));
	CHECK_UINT(med.last.codec, HAL_CODEC_G729);
	CHECK(call_fax_on(0, 0));
	CHECK_UINT(med.fax_calls, 1);
	CHECK_UINT(med.fax.codec, HAL_CODEC_PCMU);
	CHECK_UINT(ev.reinvite_done, 0);
	CHECK_UINT(ev.failed, 0);
	CHECK(find_send("BYE") < 0);
	CHECK(call_hold(0, 0));

	/* --- the 491 of a crossing re-INVITE, and the backoff behind it --- */

	CASE("fax auth: a 491 to the upspeed arms the glare timer and sends "
	     "nothing yet");
	fx_reset();
	uac_up_codec("18 101", NULL);
	CHECK(call_fax_start(0, 0));
	CHECK(req_of("INVITE", &first));
	sends = count_send("INVITE");
	reply(491, NULL, NULL, NULL);
	CHECK(timer_armed(SIP_TIMER_GLARE));
	CHECK_UINT(count_send("INVITE"), sends);
	/* an active leg that refuses an operation of its own is what
	 * c->glare_wait looks like through the surfaces a case has */
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.state, "active"));
	CHECK(!call_hold(0, 0));

	CASE("fax auth: the glare retry re-offers G.711 alone on a new CSeq");
	fake_advance(4000);
	CHECK(req_of("INVITE", &retry));
	CHECK_UINT(retry.cseq, first.cseq + 1);
	CHECK(has(retry.body, "RTP/AVP 0 8 101\r\n"));
	CHECK(!has(retry.body, "a=rtpmap:18"));
	/* the retry carries the operation the wait held, so it is the upspeed
	 * offer again and never the hold a free leg would have taken */
	CHECK(!has(retry.body, "a=sendonly"));
	CHECK_UINT(med.fax_calls, 1);

	CASE("fax auth: the glare retry is bounded and the leg keeps the "
	     "profile");
	reply(491, NULL, NULL, NULL);
	fake_advance(4000);
	CHECK(req_of("INVITE", &retry));
	CHECK_UINT(retry.cseq, first.cseq + 2);
	reply(491, NULL, NULL, NULL);
	CHECK(!timer_armed(SIP_TIMER_GLARE));
	sends = count_send("INVITE");
	fake_advance(4000);
	CHECK_UINT(count_send("INVITE"), sends);
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.state, "active"));
	CHECK(call_fax_on(0, 0));
	CHECK_UINT(med.fax_calls, 1);
	CHECK_UINT(ev.reinvite_done, 0);
	CHECK_UINT(ev.failed, 0);

	CASE("fax auth: the glare retry carries a fresh credential budget and "
	     "answers a second 401");
	fx_reset();
	uac_up_codec("18 101", NULL);
	CHECK(call_fax_start(0, 0));
	reply(401, NULL, chal, NULL);
	reply(491, NULL, NULL, NULL);
	CHECK(timer_armed(SIP_TIMER_GLARE));
	fake_advance(4000);
	CHECK(req_of("INVITE", &first));
	reply(401, NULL, chal2, NULL);
	CHECK(req_of("INVITE", &retry));
	CHECK_UINT(retry.cseq, first.cseq + 1);
	CHECK(has(last_text(), "Authorization: Digest"));
	CHECK(has(last_text(), "nonce=\"a-second-nonce\""));
	CHECK(has(retry.body, "RTP/AVP 0 8 101\r\n"));
}

static const char *t38_body(const char *addr, unsigned int port,
			    const char *audio, const char *extra);
static void t38_enable(bool on);
static const char *savp_body(const char *addr, unsigned int port, const char *pts,
			     const char *crypto, const char *extra);

/* ------------------------------------- the far end's own G.711 fax upspeed */

/* the narrowing re-INVITE a fax gateway sends, with the audio list the case
 * names; cseq 1 is the peer's first in-dialog request on this leg */
static void follow_offer(const char *pts, const char *extra)
{
	rx(uac_in_dialog("INVITE", "peer-tag", 1, NULL,
			 sdp_body(T_PEER, 20002, pts, extra)));
}

/* a G.729 call with the far end's answer tone already recorded on the leg */
static void follow_call_up(const char *pts)
{
	fx_reset();
	uac_up_codec(pts, NULL);
	call_fax_hint(0, 0);
}

/*
 * The Annex B capability, from the backend feature bit to the wire and to the
 * stream. The leg reads the bit once at its own start, exactly as it reads the
 * T.38 and SRTP ones, so each case sets it before the call is dialled.
 *
 * The advertisement is the receive-side claim and takes no config: a board with
 * the coder says annexb=yes whether or not the codec section asks for silence
 * suppression. The transmit arm takes the section AND the channel cn_enable AND
 * the peer's explicit yes, and hal_media.annexb is where the answer lands.
 */
static void g729b_up(uint32_t features, bool ss, bool cn, const char *answer_fmtp)
{
	fx_reset();
	calls_hal_features_set(features);
	cfg.channel[0].cn_enable = cn;
	cfg.codecs = 1;
	cfg.codec[0].enabled = true;
	cfg.codec[0].tr104_id = 3;
	cfg.codec[0].silence_suppression = ss;
	uac_up_codec("18 101", answer_fmtp);
}

static void test_g729_annexb(void)
{
	struct req_info ri;

	CASE("annexb: a backend with the feature offers annexb=yes");
	g729b_up(HAL_FEAT_G729 | HAL_FEAT_G729B, false, true, "a=fmtp:18 annexb=yes\r\n");
	CHECK(req_of("INVITE", &ri));
	CHECK(has(ri.body, "a=fmtp:18 annexb=yes\r\n"));

	CASE("annexb: and arms nothing while the codec section asks for none");
	CHECK(!med.last.annexb);

	CASE("annexb: the same board with silence suppression and a peer yes arms it");
	g729b_up(HAL_FEAT_G729 | HAL_FEAT_G729B, true, true, "a=fmtp:18 annexb=yes\r\n");
	CHECK(req_of("INVITE", &ri));
	CHECK(has(ri.body, "a=fmtp:18 annexb=yes\r\n"));
	CHECK(med.last.annexb);

	CASE("annexb: cn_enable off does not arm even with silence suppression and a peer yes");
	g729b_up(HAL_FEAT_G729 | HAL_FEAT_G729B, true, false, "a=fmtp:18 annexb=yes\r\n");
	CHECK(req_of("INVITE", &ri));
	CHECK(has(ri.body, "a=fmtp:18 annexb=yes\r\n"));
	CHECK(!med.last.annexb);

	CASE("annexb: a peer that answers without the fmtp does not arm it");
	g729b_up(HAL_FEAT_G729 | HAL_FEAT_G729B, true, true, NULL);
	CHECK(!med.last.annexb);

	CASE("annexb: a peer that answers no does not arm it either");
	g729b_up(HAL_FEAT_G729 | HAL_FEAT_G729B, true, true, "a=fmtp:18 annexb=no\r\n");
	CHECK(!med.last.annexb);

	/* the Annex A board: HAL_FEAT_G729 without the Annex B bit, which is
	 * what a backend whose G.729 is Annex A alone reports */
	CASE("annexb: a backend without the feature offers annexb=no");
	g729b_up(HAL_FEAT_G729, true, true, "a=fmtp:18 annexb=yes\r\n");
	CHECK(req_of("INVITE", &ri));
	CHECK(has(ri.body, "a=fmtp:18 annexb=no\r\n"));

	CASE("annexb: and never arms, whatever the section and the peer say");
	CHECK(!med.last.annexb);

	fx_reset();
	memset(&cfg.codec[0], 0, sizeof(cfg.codec[0]));
	cfg.codecs = 0;
}

/*
 * A channel the provisioning gave no G.711 at all. Its leg shares nothing with
 * the far end's narrowing offer, so the answer selection finds no codec and the
 * exchange is refused, unless the follow seats the fallback carrier first. The
 * outgoing upspeed serves the same channel through loc_g711_first(), so both
 * directions have to.
 *
 * The option is set here rather than by the caller, because the leg reads it
 * once at its own start and a case that named it later would change nothing.
 * Every caller therefore restores the config table when it is done.
 */
static void follow_g729_only_up(bool follow)
{
	fx_reset();
	cfg.channel[0].fax_upspeed_follow = follow;
	cfg.codecs = 1;
	cfg.codec[0].enabled = true;
	cfg.codec[0].tr104_id = 3;
	uac_up_codec("18 101", NULL);
}

static void fax_remote_codec_check(unsigned int tr104_id, uint8_t codec,
				   const char *pts, const char *extra,
				   const char *name, uint32_t features)
{
	struct slot_view sv;
	unsigned int configs, sends;
	bool running = false;

	cfg_default();
	fx_reset();
	codec_sections_set(tr104_id, 0);
	cfg.codec[0].silence_suppression = true;
	cfg.channel[0].cn_enable = true;
	calls_hal_features_set(features);
	uac_up_codec(pts, extra);
	CHECK_UINT(med.last.codec, codec);
	configs = med.config_calls;
	sends = fk.sends;
	CHECK(call_fax_kind_set(0, 0, HAL_PASSTHROUGH_MODEM));
	CHECK_UINT(med.config_calls, configs + 1);
	CHECK_UINT(med.last.codec, codec);
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_MODEM);
	CHECK_UINT(med.last.jb_mode, HAL_JB_FIXED);
	CHECK(!med.last.vad);
	CHECK(!med.last.cng);
	CHECK(!med.last.annexb);
	CHECK_UINT(med.fax_calls, 0);
	CHECK_UINT(fk.sends, sends);
	CHECK(!call_fax_on(0, 0));
	CHECK_UINT(call_fax_kind_get(0, 0), HAL_PASSTHROUGH_MODEM);
	CHECK_UINT(calls_line_fax_kind(0, &running), HAL_PASSTHROUGH_MODEM);
	CHECK(running);
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.codec, name));
	CHECK(!strcmp(sv.fax, "passthrough"));
	CHECK(!strcmp(sv.fax_kind, "modem"));
	cfg_default();
}

static void fax_remote_t38_offer(void)
{
	fx_reset();
	t38_enable(true);
	uac_up(NULL);
	CHECK(call_fax_kind_set(0, 0, HAL_PASSTHROUGH_MODEM));
	med.nseq = 0;
	rx(uac_in_dialog("INVITE", "peer-tag", 1, NULL,
			 t38_body(T_PEER, 30000,
				  "m=audio 0 RTP/AVP 0\r\n", NULL)));
}

static void test_fax_remote_kind(void)
{
	struct slot_view sv;
	unsigned int configs, sends;
	bool running = false;

	CASE("remote kind: PCMU takes MODEM without a profile or offer");
	fax_remote_codec_check(1, HAL_CODEC_PCMU, "0 13 101",
			       "a=rtpmap:13 CN/8000\r\n", "PCMU", 0);

	CASE("remote kind: a repeat programs nothing");
	configs = med.config_calls;
	sends = fk.sends;
	CHECK(call_fax_kind_set(0, 0, HAL_PASSTHROUGH_MODEM));
	CHECK_UINT(med.config_calls, configs);
	CHECK_UINT(fk.sends, sends);
	CHECK_UINT(med.fax_calls, 0);

	CASE("remote kind: a local preamble moves a kind-only leg to FAX");
	CHECK(call_fax_kind_set(0, 0, HAL_PASSTHROUGH_FAX));
	CHECK_UINT(med.config_calls, configs + 1);
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_FAX);
	CHECK_UINT(call_fax_kind_get(0, 0), HAL_PASSTHROUGH_FAX);
	CHECK_UINT(med.last.codec, HAL_CODEC_PCMU);
	CHECK_UINT(med.fax_calls, 0);
	CHECK_UINT(fk.sends, sends);
	CHECK(!call_fax_on(0, 0));

	CASE("remote kind: an answer tone never demotes FAX");
	configs = med.config_calls;
	CHECK(call_fax_kind_set(0, 0, HAL_PASSTHROUGH_MODEM));
	CHECK_UINT(med.config_calls, configs);
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_FAX);
	CHECK_UINT(call_fax_kind_get(0, 0), HAL_PASSTHROUGH_FAX);
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.fax_kind, "fax"));
	CHECK_UINT(fk.sends, sends);

	CASE("remote kind: G.729 keeps its codec and clears Annex B");
	fax_remote_codec_check(3, HAL_CODEC_G729, "18 13 101",
			       "a=rtpmap:13 CN/8000\r\n"
			       "a=fmtp:18 annexb=yes\r\n",
			       "G729", HAL_FEAT_G729 | HAL_FEAT_G729B);

	CASE("remote kind: G.722 keeps its codec with fixed media policy");
	fax_remote_codec_check(5, HAL_CODEC_G722, "9 13 101",
			       "a=rtpmap:9 G722/8000\r\n"
			       "a=rtpmap:13 CN/8000\r\n",
			       "G722", 0);

	CASE("remote kind: Opus keeps its codec with fixed media policy");
	fax_remote_codec_check(6, HAL_CODEC_OPUS, "96 101",
			       "a=rtpmap:96 opus/48000/2\r\n", "opus", 0);

	CASE("remote kind: G.726-32 keeps its codec with fixed media policy");
	fax_remote_codec_check(4, HAL_CODEC_G726_32, "111 13 101",
			       "a=rtpmap:111 G726-32/8000\r\n"
			       "a=rtpmap:13 CN/8000\r\n", "G726-32", 0);

	CASE("remote kind: no leg reports OFF and refuses MODEM");
	fx_reset();
	CHECK_UINT(call_fax_kind_get(0, 0), HAL_PASSTHROUGH_OFF);
	CHECK(!call_fax_kind_set(0, 0, HAL_PASSTHROUGH_MODEM));
	CHECK_UINT(med.config_calls, 0);

	CASE("remote kind: FAX needs a kind or profile");
	uac_up(NULL);
	configs = med.config_calls;
	CHECK(!call_fax_kind_set(0, 0, HAL_PASSTHROUGH_FAX));
	CHECK_UINT(med.config_calls, configs);
	CHECK_UINT(call_fax_kind_get(0, 0), HAL_PASSTHROUGH_OFF);
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!sv.fax[0]);
	CHECK(!sv.fax_kind[0]);

	CASE("remote kind: an exchange records the kind without a push");
	CHECK(call_hold(0, 0));
	configs = med.config_calls;
	sends = fk.sends;
	CHECK(call_fax_kind_set(0, 0, HAL_PASSTHROUGH_MODEM));
	CHECK_UINT(call_fax_kind_get(0, 0), HAL_PASSTHROUGH_MODEM);
	CHECK_UINT(med.config_calls, configs);
	CHECK_UINT(fk.sends, sends);
	CHECK_UINT(med.fax_calls, 0);

	CASE("remote kind: the exchange answer programs the recorded kind");
	reply(200, NULL, NULL,
	      sdp_body(T_PEER, 20000, "0", "a=recvonly\r\n"));
	CHECK_UINT(med.config_calls, configs + 1);
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_MODEM);
	CHECK_UINT(med.last.codec, HAL_CODEC_PCMU);
	CHECK_UINT(med.last.jb_mode, HAL_JB_FIXED);
	CHECK_UINT(med.fax_calls, 0);

	CASE("remote kind: a failed hold applies the deferred modem kind");
	cfg_default();
	fx_reset();
	codec_sections_set(1, 0);
	cfg.codec[0].silence_suppression = true;
	cfg.channel[0].cn_enable = true;
	uac_up_codec("0 13 101", "a=rtpmap:13 CN/8000\r\n");
	CHECK(med.last.vad);
	CHECK(med.last.cng);
	CHECK(call_hold(0, 0));
	configs = med.config_calls;
	CHECK(call_fax_kind_set(0, 0, HAL_PASSTHROUGH_MODEM));
	CHECK_UINT(med.config_calls, configs);
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_OFF);
	reply(488, NULL, NULL, NULL);
	CHECK_UINT(med.config_calls, configs + 1);
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_MODEM);
	CHECK_UINT(med.last.jb_mode, HAL_JB_FIXED);
	CHECK(!med.last.vad);
	CHECK(!med.last.cng);
	CHECK(!call_fax_on(0, 0));
	CHECK_UINT(calls_line_fax_kind(0, &running), HAL_PASSTHROUGH_MODEM);
	CHECK(running);
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.fax, "passthrough"));
	CHECK(!strcmp(sv.fax_kind, "modem"));

	CASE("remote kind: a failed hold applies the deferred fax kind");
	cfg_default();
	fx_reset();
	uac_up(NULL);
	CHECK(call_fax_kind_set(0, 0, HAL_PASSTHROUGH_MODEM));
	CHECK(call_hold(0, 0));
	configs = med.config_calls;
	CHECK(call_fax_kind_set(0, 0, HAL_PASSTHROUGH_FAX));
	CHECK_UINT(med.config_calls, configs);
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_MODEM);
	reply(488, NULL, NULL, NULL);
	CHECK_UINT(med.config_calls, configs + 1);
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_FAX);
	CHECK_UINT(call_fax_kind_get(0, 0), HAL_PASSTHROUGH_FAX);
	CHECK(!call_fax_on(0, 0));
	CHECK_UINT(calls_line_fax_kind(0, NULL), HAL_PASSTHROUGH_FAX);
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.fax_kind, "fax"));

	CASE("remote kind: a refused upspeed never pushes the old codec");
	fx_reset();
	uac_up_codec("18 101", NULL);
	CHECK_UINT(med.last.codec, HAL_CODEC_G729);
	CHECK(call_fax_start(0, 0));
	CHECK_UINT(med.fax.codec, HAL_CODEC_PCMU);
	configs = med.config_calls;
	reply(488, NULL, NULL, NULL);
	CHECK_UINT(med.config_calls, configs);
	CHECK_UINT(med.last.codec, HAL_CODEC_G729);
	CHECK(call_fax_on(0, 0));

	CASE("remote kind: a PCMA-only offer still applies the follow profile");
	follow_call_up("0 101");
	CHECK(call_fax_kind_set(0, 0, HAL_PASSTHROUGH_MODEM));
	sends = fk.sends;
	med.nseq = 0;
	follow_offer("8 101", NULL);
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(fk.sends, sends + 1);
	CHECK_UINT(med.fax_calls, 1);
	CHECK_UINT(med.fax.codec, HAL_CODEC_PCMA);
	CHECK_UINT(med.last.codec, HAL_CODEC_PCMA);
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_MODEM);
	CHECK(med_seq_index(MED_FAX) >= 0);
	CHECK(med_seq_index(MED_FAX) < med_seq_index(MED_CONFIG));
	CHECK(call_fax_on(0, 0));

	CASE("remote kind: a followed profile refuses a local upspeed");
	rx(uac_in_dialog("ACK", "peer-tag", 1, NULL, NULL));
	sends = fk.sends;
	CHECK(!call_fax_start(0, 0));
	CHECK_UINT(med.fax_calls, 1);
	CHECK_UINT(fk.sends, sends);

	CASE("remote kind: preamble start and end then follow keep FAX");
	follow_call_up("0 101");
	CHECK(call_fax_kind_set(0, 0, HAL_PASSTHROUGH_MODEM));
	CHECK(call_fax_kind_set(0, 0, HAL_PASSTHROUGH_FAX));
	configs = med.config_calls;
	/* The HAL has no edge flag, so both preamble rows use this entry. */
	CHECK(call_fax_kind_set(0, 0, HAL_PASSTHROUGH_FAX));
	CHECK_UINT(med.config_calls, configs);
	follow_offer("8 101", NULL);
	CHECK_UINT(med.fax_calls, 1);
	CHECK_UINT(med.last.codec, HAL_CODEC_PCMA);
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_FAX);
	CHECK_UINT(call_fax_kind_get(0, 0), HAL_PASSTHROUGH_FAX);
	rx(uac_in_dialog("ACK", "peer-tag", 1, NULL, NULL));
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.fax_kind, "fax"));

	CASE("remote kind: fax_upspeed_follow zero still permits the kind");
	fx_reset();
	cfg.channel[0].fax_upspeed_follow = false;
	uac_up_codec("18 101", NULL);
	call_fax_hint(0, 0);
	CHECK(call_fax_kind_set(0, 0, HAL_PASSTHROUGH_MODEM));
	CHECK_UINT(med.last.codec, HAL_CODEC_G729);
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_MODEM);
	CHECK_UINT(med.fax_calls, 0);

	CASE("remote kind: a disabled follow applies no profile");
	follow_offer("8 101", NULL);
	CHECK_UINT(med.fax_calls, 0);
	CHECK_UINT(med.last.codec, HAL_CODEC_PCMA);
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_MODEM);
	CHECK(!call_fax_on(0, 0));
	cfg_default();

	CASE("remote kind: a local answer on a kind-only leg runs the upspeed");
	fx_reset();
	uac_up_codec("18 101", NULL);
	CHECK(call_fax_kind_set(0, 0, HAL_PASSTHROUGH_MODEM));
	sends = fk.sends;
	CHECK(call_fax_start(0, 0));
	CHECK_UINT(med.fax_calls, 1);
	CHECK_UINT(med.fax.codec, HAL_CODEC_PCMU);
	CHECK_UINT(fk.sends, sends + 1);
	CHECK(call_fax_on(0, 0));
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "0 101", NULL));
	CHECK_UINT(med.last.codec, HAL_CODEC_PCMU);

	CASE("remote kind: an answered local upspeed refuses a second profile");
	sends = fk.sends;
	CHECK(!call_fax_start(0, 0));
	CHECK_UINT(med.fax_calls, 1);
	CHECK_UINT(fk.sends, sends);

	CASE("remote kind: an inbound T.38 offer behind the kind is accepted");
	fax_remote_t38_offer();
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(med.last.codec, HAL_CODEC_T38);
	CHECK(med_seq_index(MED_STOP) >= 0);
	CHECK_UINT(med.fax_calls, 0);
	rx(uac_in_dialog("ACK", "peer-tag", 1, NULL, NULL));
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.fax, "t38"));

	CASE("remote kind: a committed T.38 session records without a push");
	configs = med.config_calls;
	sends = fk.sends;
	CHECK(call_fax_kind_set(0, 0, HAL_PASSTHROUGH_FAX));
	CHECK_UINT(call_fax_kind_get(0, 0), HAL_PASSTHROUGH_FAX);
	CHECK_UINT(med.config_calls, configs);
	CHECK_UINT(med.last.codec, HAL_CODEC_T38);
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_MODEM);
	CHECK_UINT(fk.sends, sends);
	CHECK_UINT(med.fax_calls, 0);

	CASE("remote kind: a failed T.38 hold keeps the recorded fax kind");
	fax_remote_t38_offer();
	CHECK(has(last_text(), "SIP/2.0 200"));
	rx(uac_in_dialog("ACK", "peer-tag", 1, NULL, NULL));
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.fax, "t38"));
	CHECK(!call_fax_on(0, 0));
	CHECK(call_hold(0, 0));
	configs = med.config_calls;
	CHECK(call_fax_kind_set(0, 0, HAL_PASSTHROUGH_FAX));
	CHECK_UINT(med.config_calls, configs);
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_MODEM);
	reply(488, NULL, NULL, NULL);
	CHECK_UINT(med.config_calls, configs);
	CHECK_UINT(med.last.codec, HAL_CODEC_T38);
	CHECK_UINT(call_fax_kind_get(0, 0), HAL_PASSTHROUGH_FAX);
	t38_enable(false);
}

/*
 * These cases record only the hint to test the two-signal recognizer alone.
 * A hint never applies a profile; the qualified offer does, without an
 * outgoing re-INVITE. The send counts distinguish that offer from its answer.
 */
static void test_fax_follow(void)
{
	struct slot_view sv;
	struct req_info ri;
	unsigned int before;

	CASE("follow: the leg runs on G.729 before the far end's offer");
	follow_call_up("18 101");
	CHECK_UINT(med.last.codec, HAL_CODEC_G729);
	CHECK_UINT(med.fax_calls, 0);
	before = fk.sends;

	CASE("follow: the narrowing offer is answered with G.711 alone");
	follow_offer("0 8 101", NULL);
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(has(last_text(), "a=rtpmap:0 PCMU/8000"));
	CHECK(!has(last_text(), "G729"));

	/*
	 * The recognizer sends nothing of its own, and the answer this exchange
	 * owes is the one send that may appear. A count that only said "no
	 * re-INVITE" would pass on a leg that answered nothing at all.
	 */
	CASE("follow: and that answer is the ONLY thing this side put on the wire");
	CHECK_UINT(fk.sends, before + 1);
	CHECK(has(send_text(before), "SIP/2.0 200"));

	CASE("follow: the profile reached this leg's own stream");
	CHECK_UINT(med.fax_calls, 1);
	CHECK_UINT(med.fax_line, 0);
	CHECK_UINT(med.fax_stream, 0);
	CHECK_UINT(med.fax.mode, HAL_FAX_PASSTHROUGH);

	CASE("follow: on the codec the answer just selected");
	CHECK_UINT(med.fax.codec, HAL_CODEC_PCMU);
	CHECK_UINT(med.fax.codec_pt, 0);

	CASE("follow: with the canceller, both suppressors and the buffer clamped");
	CHECK(med.fax.ec_disable);
	CHECK(med.fax.vad_disable);
	CHECK(med.fax.cng_disable);
	CHECK(med.fax.silence_comp_disable);
	CHECK(med.fax.jb_fixed);

	CASE("follow: and the stream this exchange programmed carries the kind");
	CHECK_UINT(med.last.codec, HAL_CODEC_PCMU);
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_MODEM);
	CHECK_UINT(med.last.jb_mode, HAL_JB_FIXED);
	CHECK(!med.last.vad);
	CHECK(!med.last.cng);

	/* the machine carries no fax state, and no exchange of ours completed */
	CASE("follow: and reports nothing to the machine");
	CHECK_UINT(ev.reinvite_done, 0);
	CHECK_UINT(ev.failed, 0);
	CHECK_UINT(ev.hold_changed, 0);

	CASE("follow: the status dump names the passthrough and the modem kind");
	rx(uac_in_dialog("ACK", "peer-tag", 1, NULL, NULL));
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.fax, "passthrough"));
	CHECK(!strcmp(sv.fax_kind, "modem"));

	/*
	 * The profile goes down before the description that carries it, which is
	 * the order the outgoing upspeed uses for the same reason: the stream is
	 * then programmed once, with the clamp already confirmed by the backend.
	 * The recorder keeps the initial MED_CONFIG of the call setup, and the
	 * index answers the FIRST op of a kind, so the sequence is reset once
	 * the call is up.
	 */
	CASE("follow: the profile is applied BEFORE the stream is programmed");
	follow_call_up("18 101");
	med.nseq = 0;
	follow_offer("0 8 101", NULL);
	CHECK(med_seq_index(MED_FAX) >= 0);
	CHECK(med_seq_index(MED_CONFIG) >= 0);
	CHECK(med_seq_index(MED_FAX) < med_seq_index(MED_CONFIG));

	/*
	 * The codec is the one the answer selected and never the committed one.
	 * This channel prefers PCMU and the call is on G.729, so a profile built
	 * from the leg's preference would hand the DSP PCMU while the far end
	 * sends A-law.
	 */
	CASE("follow: a PCMA-only offer converts the DSP to PCMA");
	follow_call_up("18 101");
	follow_offer("8 101", NULL);
	CHECK_UINT(med.fax_calls, 1);
	CHECK_UINT(med.fax.codec, HAL_CODEC_PCMA);
	CHECK_UINT(med.fax.codec_pt, 8);

	/*
	 * And the same offer on a leg the call already committed to PCMU. This
	 * is the case a committed-codec preference gets wrong: the committed
	 * codec is a G.711, so it would win, and the DSP would decode A-law
	 * bytes as mu-law for the whole page.
	 */
	CASE("follow: even where the committed codec is the other G.711");
	follow_call_up("0 101");
	CHECK_UINT(med.last.codec, HAL_CODEC_PCMU);
	follow_offer("8 101", NULL);
	CHECK_UINT(med.fax_calls, 1);
	CHECK_UINT(med.fax.codec, HAL_CODEC_PCMA);
	CHECK_UINT(med.fax.codec_pt, 8);
	CHECK_UINT(med.last.codec, HAL_CODEC_PCMA);

	/*
	 * The tone is half the evidence and the offer is the other half. A
	 * narrowing re-INVITE on a leg that heard no answer tone is an ordinary
	 * codec renegotiation, and converting it would put every such call into
	 * a fax carrier.
	 */
	CASE("follow: the same offer without a tone leaves the leg on voice");
	fx_reset();
	uac_up_codec("18 101", NULL);
	follow_offer("0 8 101", NULL);
	CHECK_UINT(med.fax_calls, 0);
	CHECK_UINT(med.last.codec, HAL_CODEC_PCMU);
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_OFF);
	CHECK_UINT(med.last.jb_mode, HAL_JB_ADAPTIVE);

	CASE("follow: a hold offer with a tone behind it is a hold and nothing else");
	follow_call_up("18 101");
	follow_offer("0 8 101", "a=sendonly\r\n");
	CHECK_UINT(med.fax_calls, 0);
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(has(last_text(), "a=recvonly"));
	CHECK_UINT(med.last.direction, HAL_MEDIA_RECVONLY);
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_OFF);

	CASE("follow: and so is the RFC 2543 c=0.0.0.0 spelling of it");
	follow_call_up("18 101");
	rx(uac_in_dialog("INVITE", "peer-tag", 1, NULL,
			 sdp_body("0.0.0.0", 20002, "0 8 101", NULL)));
	CHECK_UINT(med.fax_calls, 0);

	/*
	 * An image offer is a different conversation: the T.38 gates own it, and
	 * a G.711 profile pushed onto a leg that is negotiating UDPTL would name
	 * a carrier the session does not have.
	 */
	CASE("follow: an image offer beside the G.711 audio is not followed");
	follow_call_up("18 101");
	follow_offer("0 8 101", "m=image 30002 udptl t38\r\n");
	CHECK_UINT(med.fax_calls, 0);
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(has(last_text(), "m=image 0 udptl t38"));

	/*
	 * And neither is the upgrade shape itself, on a leg whose channel and
	 * backend both carry T.38: the audio section is withdrawn and the image
	 * one is live, so the session this answer accepted is the image one and
	 * a G.711 profile would name a carrier it does not have.
	 */
	CASE("follow: nor is the T.38 upgrade this leg accepts");
	fx_reset();
	t38_enable(true);
	uac_up_codec("18 101", NULL);
	call_fax_hint(0, 0);
	rx(uac_in_dialog("INVITE", "peer-tag", 1, NULL,
			 t38_body(T_PEER, 30002, "m=audio 0 RTP/AVP 0 8\r\n", NULL)));
	CHECK_UINT(med.fax_calls, 0);
	rx(uac_in_dialog("ACK", "peer-tag", 1, NULL, NULL));
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.fax, "t38"));

	/* and the committed image session refuses a narrowing offer behind it,
	 * which is the gate that outlives the exchange above */
	CASE("follow: and a leg already carrying T.38 is not followed either");
	rx(uac_in_dialog("INVITE", "peer-tag", 2, NULL,
			 sdp_body(T_PEER, 20002, "0 8 101", NULL)));
	CHECK_UINT(med.fax_calls, 0);
	t38_enable(false);

	CASE("follow: a list that still names a compressed codec is not followed");
	follow_call_up("18 101");
	follow_offer("0 18 101", NULL);
	CHECK_UINT(med.fax_calls, 0);

	/*
	 * The peer repeats its upspeed after a 491 of ours, and every repeat
	 * arrives with the same narrowing. The leg has one carrier either way,
	 * so the second offer must not push the profile down again.
	 */
	CASE("follow: a repeat of the narrowing offer applies no second profile");
	follow_call_up("18 101");
	follow_offer("0 8 101", NULL);
	CHECK_UINT(med.fax_calls, 1);
	rx(uac_in_dialog("ACK", "peer-tag", 1, NULL, NULL));
	follow_offer("0 8 101", NULL);
	CHECK_UINT(med.fax_calls, 1);
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.fax, "passthrough"));

	CASE("follow: a channel with fax_upspeed_follow 0 stays on voice");
	fx_reset();
	cfg.channel[0].fax_upspeed_follow = false;
	uac_up_codec("18 101", NULL);
	call_fax_hint(0, 0);
	follow_offer("0 8 101", NULL);
	CHECK_UINT(med.fax_calls, 0);
	CHECK_UINT(med.last.codec, HAL_CODEC_PCMU);
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_OFF);
	cfg_default();

	/*
	 * The window, driven through the stamp itself rather than a clock: ten
	 * seconds of monotonic time cannot be waited out in a suite. A tone that
	 * old belongs to an earlier moment of the call, and the offer behind it
	 * is an ordinary renegotiation.
	 *
	 * The stale side is written at the edge itself, because real time only
	 * ever moves the gate's own reading forward and a stamp already past the
	 * edge stays past it. The live side keeps a margin for the same reason:
	 * a stamp written exactly at the edge would be one millisecond outside
	 * it whenever the frames below take that long.
	 */
	CASE("follow: an answer tone far older than the window is no evidence");
	follow_call_up("18 101");
	CHECK(call_fax_hint_stamp_set(0, 0, uvoice_mono_ms() -
				      CALL_FAX_HINT_WINDOW_MS - 1000));
	follow_offer("0 8 101", NULL);
	CHECK_UINT(med.fax_calls, 0);
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_OFF);

	CASE("follow: nor is one a single millisecond past the edge of it");
	follow_call_up("18 101");
	CHECK(call_fax_hint_stamp_set(0, 0, uvoice_mono_ms() -
				      CALL_FAX_HINT_WINDOW_MS - 1));
	follow_offer("0 8 101", NULL);
	CHECK_UINT(med.fax_calls, 0);

	CASE("follow: and one still inside the window converts the leg");
	follow_call_up("18 101");
	CHECK(call_fax_hint_stamp_set(0, 0, uvoice_mono_ms() -
				      CALL_FAX_HINT_WINDOW_MS + 100));
	follow_offer("0 8 101", NULL);
	CHECK_UINT(med.fax_calls, 1);
	CHECK_UINT(med.fax.codec, HAL_CODEC_PCMU);

	/*
	 * The age is an unsigned difference of two readings of one 32-bit
	 * monotonic clock, which is what carries a hint across the 2^32 wrap:
	 * the stamp is then the larger number and the difference is still the
	 * true distance. The stamp below is larger for the other reason, a
	 * reading the clock has not reached, and the same arithmetic answers it
	 * as billions of milliseconds old. A signed difference would read minus
	 * one second, pass the window and follow on evidence that does not
	 * exist yet.
	 */
	CASE("follow: a stamp the clock has not reached is no evidence either");
	follow_call_up("18 101");
	CHECK(call_fax_hint_stamp_set(0, 0, uvoice_mono_ms() + 1000));
	follow_offer("0 8 101", NULL);
	CHECK_UINT(med.fax_calls, 0);

	/*
	 * The stamp dies with the leg, the way the passthrough itself does. A
	 * hint that outlived its call would follow the first G.711 narrowing the
	 * next call on this slot ever sees.
	 */
	CASE("follow: the hint does not survive the leg it was recorded on");
	fx_reset();
	uac_up_codec("18 101", NULL);
	call_fax_hint(0, 0);
	rx(uac_in_dialog("BYE", "peer-tag", 1, NULL, NULL));
	CHECK_UINT(ev.ended, 1);
	CHECK(dial(0, 0, "1002"));
	reply(100, NULL, NULL, NULL);
	reply(200, "peer-tag2", NULL, sdp_body(T_PEER, 20004, "18 101", NULL));
	rx(uac_in_dialog("INVITE", "peer-tag2", 1, NULL,
			 sdp_body(T_PEER, 20004, "0 8 101", NULL)));
	CHECK_UINT(med.fax_calls, 0);

	CASE("follow: a slot with no leg records no hint and carries no passthrough");
	fx_reset();
	call_fax_hint(0, 0);
	CHECK(!call_fax_on(0, 0));
	CHECK(!call_fax_hint_stamp_set(0, 0, 1));

	/* the entry answers the leg's own state, which is what line/line.c
	 * reads in place of a latch it cannot take for a followed leg */
	CASE("follow: and it answers true once a leg carries the passthrough");
	follow_call_up("18 101");
	CHECK(!call_fax_on(0, 0));
	follow_offer("0 8 101", NULL);
	CHECK(call_fax_on(0, 0));

	/*
	 * The refusal path: the backend would not take the profile, so this side
	 * is not carrying a fax and must not report one. The exchange itself
	 * still stands, because the answer went out before any of this ran.
	 */
	CASE("follow: a backend that refuses the profile leaves an ordinary leg");
	follow_call_up("18 101");
	med.fax_ret = HAL_UNSUPPORTED;
	follow_offer("0 8 101", NULL);
	CHECK_UINT(med.fax_calls, 1);
	CHECK(!call_fax_on(0, 0));
	CHECK_UINT(med.last.codec, HAL_CODEC_PCMU);
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_OFF);
	CHECK_UINT(med.last.jb_mode, HAL_JB_ADAPTIVE);
	CHECK_UINT(ev.failed, 0);
	med.fax_ret = HAL_OK;

	/* --- the channel that names no G.711 at all --- */

	/*
	 * Such a leg shares no codec with the far end's narrowing offer, so the
	 * answer selection would refuse the exchange the recognizer wants to
	 * follow and the whole feature would be off on exactly the provisioning
	 * that needs it most: a G.729 carrier is the one a fax cannot cross.
	 * The recognizer's verdict is therefore read once before the answer is
	 * built, and the fallback carrier of the outgoing upspeed is seated
	 * into the leg's own list.
	 */
	CASE("follow: a G.729-only channel offers G.729 alone before the upspeed");
	follow_g729_only_up(true);
	CHECK(req_of("INVITE", &ri));
	CHECK(has(ri.body, "a=rtpmap:18 G729/8000"));
	CHECK(!has(ri.body, "PCMU"));
	CHECK_UINT(med.last.codec, HAL_CODEC_G729);
	call_fax_hint(0, 0);
	before = fk.sends;

	CASE("follow: and it answers the narrowing offer with the fallback G.711");
	follow_offer("0 101", NULL);
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(has(last_text(), "a=rtpmap:0 PCMU/8000"));
	CHECK(!has(last_text(), "G729"));

	CASE("follow: with the profile on that same codec and no offer of ours");
	CHECK_UINT(med.fax_calls, 1);
	CHECK_UINT(med.fax.mode, HAL_FAX_PASSTHROUGH);
	CHECK_UINT(med.fax.codec, HAL_CODEC_PCMU);
	CHECK_UINT(med.fax.codec_pt, 0);
	CHECK_UINT(fk.sends, before + 1);
	CHECK_UINT(med.last.codec, HAL_CODEC_PCMU);
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_MODEM);

	/*
	 * The seat is a widening and the clamp behind it is the narrowing, so
	 * the leg ends on the list the outgoing upspeed leaves on this same
	 * channel: the fallback G.711 alone, with the telephone-event beside it.
	 */
	CASE("follow: and the leg's later offers carry that G.711 alone");
	rx(uac_in_dialog("ACK", "peer-tag", 1, NULL, NULL));
	CHECK(call_hold(0, 0));
	CHECK(last_req(&ri));
	CHECK(has(ri.body, "RTP/AVP 0 101\r\n"));
	CHECK(!has(ri.body, "G729"));

	/*
	 * Without the tone the offer is an ordinary renegotiation, and the leg
	 * refuses it exactly as it did before this path existed. The list is
	 * what proves the seat never ran: a widened one would answer the same
	 * offer next time.
	 */
	CASE("follow: the same channel without a tone refuses the offer 488");
	follow_g729_only_up(true);
	follow_offer("0 101", NULL);
	CHECK(has(last_text(), "SIP/2.0 488"));
	CHECK_UINT(med.fax_calls, 0);
	CHECK_UINT(med.last.codec, HAL_CODEC_G729);

	CASE("follow: and its list still names G.729 alone");
	CHECK(call_hold(0, 0));
	CHECK(last_req(&ri));
	CHECK(has(ri.body, "a=rtpmap:18 G729/8000"));
	CHECK(!has(ri.body, "PCMU"));

	/* the knob keeps the whole feature off, the seat included: a channel
	 * that does not follow must not widen its list for an offer it refuses */
	CASE("follow: fax_upspeed_follow 0 refuses it with the tone as well");
	follow_g729_only_up(false);
	call_fax_hint(0, 0);
	follow_offer("0 101", NULL);
	CHECK(has(last_text(), "SIP/2.0 488"));
	CHECK_UINT(med.fax_calls, 0);

	CASE("follow: and that list is untouched too");
	CHECK(call_hold(0, 0));
	CHECK(last_req(&ri));
	CHECK(has(ri.body, "a=rtpmap:18 G729/8000"));
	CHECK(!has(ri.body, "PCMU"));
	cfg_default();

	/*
	 * The seat belongs to the exchange that carries it. An answer that never
	 * reached the wire leaves the leg holding the list it found, or its next
	 * offer would advertise a codec this channel does not provision and no
	 * exchange ever agreed to.
	 */
	CASE("follow: an answer that could not be sent leaves the list alone");
	follow_g729_only_up(true);
	call_fax_hint(0, 0);
	fk.send_fail = true;
	follow_offer("0 101", NULL);
	fk.send_fail = false;
	CHECK_UINT(med.fax_calls, 0);
	CHECK(call_hold(0, 0));
	CHECK(last_req(&ri));
	CHECK(has(ri.body, "a=rtpmap:18 G729/8000"));
	CHECK(!has(ri.body, "PCMU"));

	/*
	 * The other way an exchange can fail behind a seated list, and the one
	 * a peer can produce: the offer narrowed to G.711 and moved its
	 * transport at the same time. The recognizer reads the codec list and
	 * not the transport, so the verdict stands and the answer refuses.
	 */
	CASE("follow: an SAVP narrowing offer is refused with the list restored");
	follow_g729_only_up(true);
	call_fax_hint(0, 0);
	rx(uac_in_dialog("INVITE", "peer-tag", 1, NULL,
			 savp_body(T_PEER, 20002, "0 101", NULL, NULL)));
	CHECK(has(last_text(), "SIP/2.0 488"));
	CHECK_UINT(med.fax_calls, 0);
	CHECK(call_hold(0, 0));
	CHECK(last_req(&ri));
	CHECK(has(ri.body, "a=rtpmap:18 G729/8000"));
	CHECK(!has(ri.body, "PCMU"));
	cfg_default();

	/*
	 * The order of the challenged upspeed, with the far end narrowing while
	 * this leg's own credentialed retry is still in flight. rx_reinvite()
	 * reads the state alone and answers 491 for every state but ACTIVE, so
	 * the recognizer never sees the offer and the 10 s window never enters
	 * it. The profile of the local upspeed still stands.
	 */
	CASE("follow: a narrowing offer under the leg's own 401 retry is "
	     "answered 491");
	follow_call_up("18 101");
	CHECK(call_fax_start(0, 0));
	reply(401, NULL, "WWW-Authenticate: Digest realm=\"" T_DOMAIN
	      "\", nonce=\"" T_NONCE "\"\r\n", NULL);
	CHECK(has(last_text(), "Authorization: Digest"));
	follow_offer("0 8 101", NULL);
	CHECK(has(last_text(), "SIP/2.0 491"));
	CHECK_UINT(med.fax_calls, 1);
	CHECK(call_fax_on(0, 0));
}

/*
 * The line-wide kind the lec row of "voice status" resolves its canceller
 * policy from (call.h calls_line_fax_kind). It answers the same question
 * udsp/src/core/stream.c stream_line_passthrough() answers inside the library,
 * so the two gates it applies are that function's: a stream that is running,
 * and a leg that is not holding its uplink silent. A row that ignored either
 * would name a policy the DSP is not running.
 */
/* ------------------------------------ the return to voice of a false tone */

/*
 * The leg a plain answer tone armed: the tone row the rule counts, then the
 * modem kind the shell sets behind it. The two are separate entries and the
 * shell calls them in this order, so the arming candidate is recorded on a
 * leg that still carries nothing.
 */
static void exit_tone_arm(uint16_t code, uint8_t dir)
{
	CHECK_UINT(call_fax_tone_note(0, 0, code, dir, false, 0),
		   CALL_FAX_TONE_NOTED);
	CHECK(call_fax_kind_set(0, 0, HAL_PASSTHROUGH_MODEM));
}

/* the end row of that tone, with the duration the detector read for it */
static enum call_fax_tone exit_tone_end(uint16_t code, uint8_t dir, uint16_t ms)
{
	return call_fax_tone_note(0, 0, code, dir, true, ms);
}

/* a kind-only leg on G.729 with the far end's answer tone already stamped */
static void exit_kind_leg_up(void)
{
	fx_reset();
	uac_up_codec("18 101", NULL);
	call_fax_hint(0, 0);
	exit_tone_arm(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE);
}

/* the handle the line shell holds across the deferral */
static uint32_t exit_handle(void)
{
	return call_leg_handle_get(0, 0);
}

/* the SDES helpers of test_srtp() below, because a rekey is the one exchange
 * the return to voice must leave alone, with the RFC 4568 spelling of the
 * suite they take as written */
#define SUITE_EXIT_80	"AES_CM_128_HMAC_SHA1_80"
static const char *srtp_crypto_line(unsigned int tag, const char *suite,
				    const uint8_t *key);
static void srtp_uas_up(const char *call_id, const uint8_t *peer_key);

/*
 * The rule of call.h call_fax_exit. The deferral itself belongs to the line
 * shell, so these cases drive the tone rows and the exit directly. They check
 * the verdict, the order of the actions and the state that either answer
 * leaves behind.
 */
static void test_fax_exit(void)
{
	uint8_t peer[SDP_SRTP_KEY_LEN], peer2[SDP_SRTP_KEY_LEN];
	unsigned int configs, stops, starts, i;
	struct slot_view sv;
	struct req_info ri;
	uint32_t handle;
	bool running;

	CASE("exit: a 900 ms answer tone takes the leg back to voice");
	exit_kind_leg_up();
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_MODEM);
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE, 900),
		   CALL_FAX_TONE_ENDED);
	CHECK(call_fax_exit(exit_handle()));

	CASE("exit: the stream carries no kind and the adaptive buffer again");
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_OFF);
	CHECK_UINT(med.last.jb_mode, HAL_JB_ADAPTIVE);
	CHECK_UINT(med.last.codec, HAL_CODEC_G729);

	CASE("exit: and the line's tone disabler was released once");
	CHECK_UINT(med.latch_clears, 1);
	CHECK_UINT(med.latch_line, 0);

	CASE("exit: the status names no fax at all");
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!sv.fax[0]);
	CHECK(!sv.fax_kind[0]);
	CHECK_UINT(call_fax_kind_get(0, 0), HAL_PASSTHROUGH_OFF);
	CHECK(!call_fax_on(0, 0));

	/*
	 * The stamp went with the kind, so the far end's narrowing offer is an
	 * ordinary renegotiation from here. A leg that kept the stamp would
	 * follow the very offer the rule just decided was not a fax.
	 */
	CASE("exit: a G.711-only offer a second later is not followed");
	fake_advance(1000);
	follow_offer("0 101", NULL);
	CHECK_UINT(med.fax_calls, 0);
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_OFF);
	CHECK(!call_fax_on(0, 0));

	/* T.30 4.1.1 and V.25 2.2: a real answer tone runs 2.6 s at least */
	CASE("exit: a 3500 ms answer tone keeps the kind");
	exit_kind_leg_up();
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE, 3500),
		   CALL_FAX_TONE_ENDED);
	CHECK(!call_fax_exit(exit_handle()));
	CHECK_UINT(call_fax_kind_get(0, 0), HAL_PASSTHROUGH_MODEM);
	CHECK_UINT(med.latch_clears, 0);

	/*
	 * A lost packet splits one real tone into two reports once the
	 * concealment is off. Neither reading reaches the floor and their sum
	 * does, which is why the rule sums them. No row stands between the two
	 * ends here, so the sum is the whole of what refuses the exit.
	 */
	CASE("exit: two end rows of 900 and 1200 ms keep the kind");
	exit_kind_leg_up();
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE, 900),
		   CALL_FAX_TONE_ENDED);
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE, 1200),
		   CALL_FAX_TONE_ENDED);
	CHECK(!call_fax_exit(exit_handle()));
	CHECK_UINT(call_fax_kind_get(0, 0), HAL_PASSTHROUGH_MODEM);

	/* the same loss gap as the detector really reports it: the resumed tone
	 * opens with a start row, which counts as evidence of its own */
	CASE("exit: a resumed tone between the two ends keeps it as well");
	exit_kind_leg_up();
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE, 900),
		   CALL_FAX_TONE_ENDED);
	exit_tone_arm(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE);
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE, 600),
		   CALL_FAX_TONE_ENDED);
	CHECK(!call_fax_exit(exit_handle()));
	CHECK_UINT(call_fax_kind_get(0, 0), HAL_PASSTHROUGH_MODEM);

	CASE("exit: an end row on the other feed adds nothing to the total");
	exit_kind_leg_up();
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 3500),
		   CALL_FAX_TONE_END);
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE, 900),
		   CALL_FAX_TONE_ENDED);
	CHECK(call_fax_exit(exit_handle()));

	CASE("exit: an end row of another tone answers END and adds nothing");
	exit_kind_leg_up();
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_ANSAM, HAL_TONE_DIR_REMOTE, 3500),
		   CALL_FAX_TONE_END);
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE, 900),
		   CALL_FAX_TONE_ENDED);
	CHECK(call_fax_exit(exit_handle()));

	/* --- the evidence that vetoes the exit --- */

	CASE("exit: a modem carrier behind the tone keeps the kind");
	exit_kind_leg_up();
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE, 900),
		   CALL_FAX_TONE_ENDED);
	CHECK_UINT(call_fax_tone_note(0, 0, HAL_TONE_DET_MODEM,
				      HAL_TONE_DIR_REMOTE, false, 0),
		   CALL_FAX_TONE_NOTED);
	CHECK(!call_fax_exit(exit_handle()));
	CHECK_UINT(call_fax_kind_get(0, 0), HAL_PASSTHROUGH_MODEM);

	CASE("exit: a V.21 preamble keeps it, on either feed");
	exit_kind_leg_up();
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE, 900),
		   CALL_FAX_TONE_ENDED);
	call_fax_tone_note(0, 0, HAL_TONE_DET_V21_PREAMBLE, HAL_TONE_DIR_LOCAL,
			   false, 0);
	CHECK(!call_fax_exit(exit_handle()));

	CASE("exit: an ANSam start row keeps it");
	exit_kind_leg_up();
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE, 900),
		   CALL_FAX_TONE_ENDED);
	call_fax_tone_note(0, 0, HAL_TONE_DET_ANSAM, HAL_TONE_DIR_REMOTE,
			   false, 0);
	CHECK(!call_fax_exit(exit_handle()));

	CASE("exit: a second answer tone keeps it");
	exit_kind_leg_up();
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE, 900),
		   CALL_FAX_TONE_ENDED);
	call_fax_tone_note(0, 0, HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE,
			   false, 0);
	CHECK(!call_fax_exit(exit_handle()));

	/* a CNG belongs to a calling terminal and says nothing about the tone
	 * this leg heard answer it */
	CASE("exit: a CNG row is no evidence and the leg still exits");
	exit_kind_leg_up();
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE, 900),
		   CALL_FAX_TONE_ENDED);
	call_fax_tone_note(0, 0, HAL_TONE_DET_CNG, HAL_TONE_DIR_REMOTE,
			   false, 0);
	CHECK(call_fax_exit(exit_handle()));

	CASE("exit: a followed far-end upspeed keeps the kind");
	exit_kind_leg_up();
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE, 900),
		   CALL_FAX_TONE_ENDED);
	follow_offer("0 101", NULL);
	CHECK_UINT(med.fax_calls, 1);
	CHECK(!call_fax_exit(exit_handle()));
	CHECK(call_fax_on(0, 0));

	/* the far end's own conversion, whichever branch of the recognizer
	 * answers it: a leg already in the passthrough reads CARRIED */
	CASE("exit: a carried far-end upspeed keeps the kind too");
	follow_g729_only_up(true);
	call_fax_tone_note(0, 0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL,
			   false, 0);
	CHECK(call_fax_start(0, 0));
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "0", NULL));
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 900),
		   CALL_FAX_TONE_ENDED);
	follow_offer("0 101", NULL);
	rx(uac_in_dialog("ACK", "peer-tag", 1, NULL, NULL));
	CHECK(!call_fax_exit(exit_handle()));
	CHECK(call_fax_on(0, 0));
	CHECK_UINT(med.latch_clears, 0);
	cfg_default();

	CASE("exit: an accepted T.38 session keeps the kind");
	fx_reset();
	t38_enable(true);
	uac_up(NULL);
	exit_tone_arm(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE);
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE, 900),
		   CALL_FAX_TONE_ENDED);
	med.nseq = 0;
	rx(uac_in_dialog("INVITE", "peer-tag", 1, NULL,
			 t38_body(T_PEER, 30000,
				  "m=audio 0 RTP/AVP 0\r\n", NULL)));
	rx(uac_in_dialog("ACK", "peer-tag", 1, NULL, NULL));
	CHECK(!call_fax_exit(exit_handle()));
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.fax, "t38"));
	t38_enable(false);

	/*
	 * The image session this leg never heard a tone for. A peer may upgrade
	 * a plain voice call straight to T.38, and an answer tone on the leg
	 * behind it is one more row of a fax that is already running. A rule
	 * that read the tone alone would release the line's disabler under a
	 * page and put the canceller back on the carrier.
	 */
	/*
	 * The window between the 2xx and the ACK. The answer already named the
	 * image session and the stream is already carrying UDPTL, and the
	 * acceptance commits only at the ACK: a leg reading the committed view
	 * alone would arm a candidate here and take a running page back to
	 * voice.
	 */
	CASE("exit: a tone between the image 2xx and its ACK arms nothing");
	fx_reset();
	t38_enable(true);
	uac_up(NULL);
	rx(uac_in_dialog("INVITE", "peer-tag", 1, NULL,
			 t38_body(T_PEER, 30000,
				  "m=audio 0 RTP/AVP 0\r\n", NULL)));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(med.last.codec, HAL_CODEC_T38);
	CHECK_UINT(call_fax_kind_get(0, 0), HAL_PASSTHROUGH_OFF);
	exit_tone_arm(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE);
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE, 900),
		   CALL_FAX_TONE_END);
	CHECK(!call_fax_exit(exit_handle()));
	CHECK_UINT(med.latch_clears, 0);
	CHECK_UINT(med.last.codec, HAL_CODEC_T38);
	rx(uac_in_dialog("ACK", "peer-tag", 1, NULL, NULL));
	t38_enable(false);

	CASE("exit: a tone on a direct image session arms nothing");
	fx_reset();
	t38_enable(true);
	uac_up(NULL);
	rx(uac_in_dialog("INVITE", "peer-tag", 1, NULL,
			 t38_body(T_PEER, 30000,
				  "m=audio 0 RTP/AVP 0\r\n", NULL)));
	rx(uac_in_dialog("ACK", "peer-tag", 1, NULL, NULL));
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.fax, "t38"));
	CHECK_UINT(call_fax_kind_get(0, 0), HAL_PASSTHROUGH_OFF);
	exit_tone_arm(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE);
	/* the start row counted as one more row of a fax already running, so
	 * its end row is not an arming tone ending */
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE, 900),
		   CALL_FAX_TONE_END);
	CHECK(!call_fax_exit(exit_handle()));
	CHECK_UINT(med.latch_clears, 0);
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.fax, "t38"));
	t38_enable(false);

	/*
	 * The upgrade this side offered itself, which is the one path to an
	 * image session that leaves the arming tone standing: the tone armed
	 * the leg, the profile and the upgrade followed it, and nothing on the
	 * way sets evidence. The session is what refuses here.
	 */
	CASE("exit: an image session of this leg's own is never left");
	fx_reset();
	t38_enable(true);
	uac_up_codec("18 101", NULL);
	call_fax_tone_note(0, 0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL,
			   false, 0);
	CHECK(call_fax_start(0, 0));
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "0 101", NULL));
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 900),
		   CALL_FAX_TONE_ENDED);
	CHECK_UINT(call_t38_start(0, 0), CALL_T38_STARTED);
	reply(200, NULL, NULL, t38_body(T_PEER, 20000, NULL, NULL));
	CHECK_UINT(med.last.codec, HAL_CODEC_T38);
	CHECK(!call_fax_exit(exit_handle()));
	CHECK_UINT(med.latch_clears, 0);
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.fax, "t38"));
	t38_enable(false);

	/* --- the two boundaries --- */

	CASE("exit: an end row one millisecond under the floor still exits");
	exit_kind_leg_up();
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE,
				 CALL_FAX_TONE_MIN_MS - 1),
		   CALL_FAX_TONE_ENDED);
	CHECK(call_fax_exit(exit_handle()));

	CASE("exit: an end row at the floor keeps the kind");
	exit_kind_leg_up();
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE,
				 CALL_FAX_TONE_MIN_MS), CALL_FAX_TONE_ENDED);
	CHECK(!call_fax_exit(exit_handle()));
	CHECK_UINT(call_fax_kind_get(0, 0), HAL_PASSTHROUGH_MODEM);

	/*
	 * The line shell cancels the deferral on an evidence row, and the leg
	 * vetoes the exit on the same row: the two halves answer one question,
	 * so a row that arrives before the expiry stops the exit here too.
	 */
	CASE("exit: evidence in front of the exit vetoes it");
	exit_kind_leg_up();
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE, 900),
		   CALL_FAX_TONE_ENDED);
	call_fax_tone_note(0, 0, HAL_TONE_DET_MODEM, HAL_TONE_DIR_LOCAL,
			   false, 0);
	CHECK(!call_fax_exit(exit_handle()));

	CASE("exit: and a row behind the exit does not undo it");
	CHECK_UINT(call_fax_kind_get(0, 0), HAL_PASSTHROUGH_MODEM);
	exit_kind_leg_up();
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE, 900),
		   CALL_FAX_TONE_ENDED);
	CHECK(call_fax_exit(exit_handle()));
	call_fax_tone_note(0, 0, HAL_TONE_DET_MODEM, HAL_TONE_DIR_REMOTE,
			   false, 0);
	CHECK_UINT(call_fax_kind_get(0, 0), HAL_PASSTHROUGH_OFF);
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_OFF);

	/* --- the tones that never arm the rule --- */

	/*
	 * ANSam ends on the second CM sequence, one to two seconds into the
	 * tone (V.8 8.2.2, T.30 6.1.1), and ANSbar carries the phase reversals
	 * a modem asks a canceller to stand down for. Neither says anything by
	 * being short.
	 */
	/* the ANSam is the arming tone of this leg and no other row stands
	 * behind it, so the code gate is the only thing that can refuse */
	CASE("exit: a 1300 ms ANSam never returns the leg to voice");
	fx_reset();
	uac_up_codec("18 101", NULL);
	exit_tone_arm(HAL_TONE_DET_ANSAM, HAL_TONE_DIR_REMOTE);
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_ANSAM, HAL_TONE_DIR_REMOTE,
				 1300), CALL_FAX_TONE_ENDED);
	CHECK(!call_fax_exit(exit_handle()));
	CHECK_UINT(call_fax_kind_get(0, 0), HAL_PASSTHROUGH_MODEM);
	CHECK_UINT(med.latch_clears, 0);

	CASE("exit: nor a 1300 ms ANSbar, armed the same way");
	fx_reset();
	uac_up_codec("18 101", NULL);
	exit_tone_arm(HAL_TONE_DET_ANSBAR, HAL_TONE_DIR_REMOTE);
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_ANSBAR, HAL_TONE_DIR_REMOTE,
				 1300), CALL_FAX_TONE_ENDED);
	CHECK(!call_fax_exit(exit_handle()));
	CHECK_UINT(call_fax_kind_get(0, 0), HAL_PASSTHROUGH_MODEM);
	CHECK_UINT(med.latch_clears, 0);

	/* --- the receiver, whose exit lands under its own upspeed --- */

	/*
	 * The answering side runs the upspeed itself, so its exit meets an
	 * exchange in flight. The answer of that exchange selects from the
	 * leg's codec list, which the clamp reduced to the G.711 the profile
	 * took, so the restore has to wait for it: a channel provisioned with
	 * G.729 alone would otherwise refuse the PCMU 200 it asked for.
	 */
	CASE("exit: a local answer tone on a G.729-only channel converts");
	follow_g729_only_up(true);
	CHECK_UINT(call_fax_tone_note(0, 0, HAL_TONE_DET_CED,
				      HAL_TONE_DIR_LOCAL, false, 0),
		   CALL_FAX_TONE_NOTED);
	CHECK(call_fax_start(0, 0));
	CHECK_UINT(med.fax_calls, 1);

	CASE("exit: its 900 ms end row under the upspeed defers the restore");
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 900),
		   CALL_FAX_TONE_ENDED);
	configs = med.config_calls;
	CHECK(call_fax_exit(exit_handle()));
	CHECK_UINT(med.config_calls, configs);
	CHECK(!call_fax_on(0, 0));
	CHECK_UINT(call_fax_kind_get(0, 0), HAL_PASSTHROUGH_OFF);
	CHECK_UINT(med.latch_clears, 1);

	CASE("exit: the PCMU answer is still accepted and carries no kind");
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "0", NULL));
	CHECK_UINT(med.config_calls, configs + 1);
	CHECK_UINT(med.last.codec, HAL_CODEC_PCMU);
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_OFF);
	CHECK_UINT(med.last.jb_mode, HAL_JB_ADAPTIVE);

	CASE("exit: and the codec list is the one the clamp found");
	CHECK(call_hold(0, 0));
	CHECK(last_req(&ri));
	CHECK(has(ri.body, "a=rtpmap:18 G729/8000"));
	CHECK(!has(ri.body, "a=rtpmap:0 PCMU/8000"));
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "18", "a=recvonly\r\n"));
	cfg_default();

	/*
	 * The other outcome of the same exchange. reinvite_fail() rolls the
	 * offer back and pushes no media at all, and the stream is still
	 * running the profile the DSP took, so the completion has to program
	 * the committed description even though the cache already describes it.
	 */
	CASE("exit: a 488 to that upspeed forces the committed codec back");
	follow_g729_only_up(true);
	call_fax_tone_note(0, 0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL,
			   false, 0);
	CHECK(call_fax_start(0, 0));
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 900),
		   CALL_FAX_TONE_ENDED);
	CHECK(call_fax_exit(exit_handle()));
	configs = med.config_calls;
	reply(488, NULL, NULL, NULL);
	CHECK_UINT(med.config_calls, configs + 1);
	CHECK_UINT(med.last.codec, HAL_CODEC_G729);
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_OFF);
	CHECK_UINT(med.last.jb_mode, HAL_JB_ADAPTIVE);

	CASE("exit: and that leg negotiates its own codec list again");
	CHECK(call_hold(0, 0));
	CHECK(last_req(&ri));
	CHECK(has(ri.body, "a=rtpmap:18 G729/8000"));
	CHECK(!has(ri.body, "a=rtpmap:0 PCMU/8000"));
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "18", "a=recvonly\r\n"));
	cfg_default();

	/*
	 * The completion can refuse too. The fields the pending exit cleared
	 * are kept on the leg for exactly this: the stream is still running the
	 * profile the DSP took, so a status that read voice over it would
	 * describe a call nothing is carrying.
	 */
	CASE("exit: a refused pending exit keeps the fax fields");
	follow_g729_only_up(true);
	call_fax_tone_note(0, 0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL,
			   false, 0);
	CHECK(call_fax_start(0, 0));
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 900),
		   CALL_FAX_TONE_ENDED);
	CHECK(call_fax_exit(exit_handle()));
	configs = med.config_calls;
	med.config_ret = HAL_TRANSIENT;
	reply(488, NULL, NULL, NULL);
	CHECK_UINT(med.config_calls, configs + 1);
	med.config_ret = HAL_OK;
	CHECK(call_fax_on(0, 0));
	CHECK_UINT(call_fax_kind_get(0, 0), HAL_PASSTHROUGH_MODEM);
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.fax, "passthrough"));
	CHECK(!strcmp(sv.fax_kind, "modem"));

	/* and the list stays clamped with them, because the leg is carrying
	 * the passthrough again */
	CASE("exit: and its codec list stays where the clamp left it");
	CHECK(call_hold(0, 0));
	CHECK(last_req(&ri));
	CHECK(has(ri.body, "a=rtpmap:0 PCMU/8000"));
	CHECK(!has(ri.body, "a=rtpmap:18 G729/8000"));
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "0", "a=recvonly\r\n"));
	cfg_default();

	/*
	 * A peer exchange this side already answered 2xx. Its answer is built,
	 * so the leg's codec list is free, and the ACK programs the committed
	 * description with no kind in it: the restore is all the completion
	 * owes, and the ACK is where it runs.
	 */
	CASE("exit: a peer exchange completes the pending exit at its ACK");
	follow_g729_only_up(true);
	call_fax_tone_note(0, 0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL,
			   false, 0);
	CHECK(call_fax_start(0, 0));
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "0", NULL));
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 900),
		   CALL_FAX_TONE_ENDED);
	rx(uac_in_dialog("INVITE", "peer-tag", 1, NULL, NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(call_fax_exit(exit_handle()));
	CHECK(!call_fax_on(0, 0));
	rx(uac_in_dialog("ACK", "peer-tag", 1, NULL,
			 sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_OFF);

	CASE("exit: and the codec list is back behind that ACK");
	CHECK(call_hold(0, 0));
	CHECK(last_req(&ri));
	CHECK(has(ri.body, "a=rtpmap:18 G729/8000"));
	CHECK(!has(ri.body, "a=rtpmap:0 PCMU/8000"));
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "18", "a=recvonly\r\n"));
	cfg_default();

	/*
	 * The ACK's own push can be refused, and then the stream is still
	 * running the carrier. The completion is told which it was: it retries
	 * the description once and puts the fax fields back when that is
	 * refused too, so the status keeps describing the call the backend is
	 * actually carrying.
	 */
	CASE("exit: a refused ACK push puts the pending fax fields back");
	follow_g729_only_up(true);
	call_fax_tone_note(0, 0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL,
			   false, 0);
	CHECK(call_fax_start(0, 0));
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "0", NULL));
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 900),
		   CALL_FAX_TONE_ENDED);
	rx(uac_in_dialog("INVITE", "peer-tag", 1, NULL, NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(call_fax_exit(exit_handle()));
	configs = med.config_calls;
	med.config_ret = HAL_TRANSIENT;
	rx(uac_in_dialog("ACK", "peer-tag", 1, NULL,
			 sdp_body(T_PEER, 20000, "0", NULL)));
	/* the ACK's own push and the one retry the completion owes it */
	CHECK_UINT(med.config_calls, configs + 2);
	CHECK_UINT(ev.failed, 0);
	med.config_ret = HAL_OK;
	CHECK(call_fax_on(0, 0));
	CHECK_UINT(call_fax_kind_get(0, 0), HAL_PASSTHROUGH_MODEM);
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.fax, "passthrough"));
	CHECK(!strcmp(sv.fax_kind, "modem"));

	CASE("exit: and that leg keeps the clamped list with them");
	CHECK(call_hold(0, 0));
	CHECK(last_req(&ri));
	CHECK(has(ri.body, "a=rtpmap:0 PCMU/8000"));
	CHECK(!has(ri.body, "a=rtpmap:18 G729/8000"));
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "0", "a=recvonly\r\n"));
	cfg_default();

	/*
	 * The completion's retry can be the push that stops the stream. The
	 * ACK carried a new receive key and its own push failed at the stop, so
	 * the cache still names the old key and the retry finds a rekey: it
	 * stops, configures and has to start again. A push that could not start
	 * would leave this leg active with its media down.
	 */
	CASE("exit: a retry that rekeys the stream starts it again");
	fx_reset();
	cfg.srtp_enabled = true;
	calls_hal_features_set(HAL_FEAT_SRTP);
	for (i = 0; i < SDP_SRTP_KEY_LEN; i++) {
		peer[i] = (uint8_t)(0x40 + i);
		peer2[i] = (uint8_t)(0x80 + i);
	}
	srtp_uas_up("call-fax-exit-ack-rekey", peer);
	exit_tone_arm(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE);
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE, 900),
		   CALL_FAX_TONE_ENDED);
	rx(uas_in_dialog("INVITE", 2, NULL, NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(call_fax_exit(exit_handle()));
	starts = med.start_calls;
	stops = med.stop_calls;
	/* both attempts of the ACK's own stop refuse, so that push leaves the
	 * cache and the running stream exactly as it found them */
	med.stop_transient = 2;
	med.nseq = 0;
	rx(uas_in_dialog("ACK", 2, NULL,
			 savp_body(T_PEER, 20000, "0 101",
				   srtp_crypto_line(1, SUITE_EXIT_80, peer2),
				   NULL)));
	CHECK_UINT(ev.failed, 0);
	CHECK_UINT(med.stop_calls, stops + 3);
	CHECK_UINT(med.start_calls, starts + 1);
	CHECK(!memcmp(med.last.srtp.key_rx, peer2, SDP_SRTP_KEY_LEN));
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_OFF);

	CASE("exit: and the line carries running media behind it");
	running = false;
	calls_line_fax_kind(0, &running);
	CHECK(running);
	cfg_default();
	calls_hal_features_set(0);

	/*
	 * The same rule for the kind entry, which re-programs the committed
	 * description too. The rekey is left pending by an ACK whose stop
	 * refused, and the answer tone behind it is what finds it.
	 */
	CASE("exit: a kind change that rekeys the stream starts it again");
	fx_reset();
	cfg.srtp_enabled = true;
	calls_hal_features_set(HAL_FEAT_SRTP);
	srtp_uas_up("call-fax-kind-rekey", peer);
	med.stop_transient = 2;
	rx(uas_in_dialog("INVITE", 2, NULL, NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));
	rx(uas_in_dialog("ACK", 2, NULL,
			 savp_body(T_PEER, 20000, "0 101",
				   srtp_crypto_line(1, SUITE_EXIT_80, peer2),
				   NULL)));
	CHECK_UINT(ev.failed, 0);
	/* the refused stop left the cache on the key the stream still runs */
	CHECK(!memcmp(med.last.srtp.key_rx, peer, SDP_SRTP_KEY_LEN));
	starts = med.start_calls;
	stops = med.stop_calls;
	med.nseq = 0;
	exit_tone_arm(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE);
	CHECK_UINT(med.stop_calls, stops + 1);
	CHECK_UINT(med.start_calls, starts + 1);
	CHECK(med_seq_index(MED_STOP) == 0);
	CHECK(med_seq_index(MED_CONFIG) == 1);
	CHECK(med_seq_index(MED_START) == 2);
	CHECK(!memcmp(med.last.srtp.key_rx, peer2, SDP_SRTP_KEY_LEN));
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_MODEM);
	running = false;
	calls_line_fax_kind(0, &running);
	CHECK(running);
	cfg_default();
	calls_hal_features_set(0);

	/*
	 * The refusal behind that stop. The stream is down and the description
	 * the exchange agreed never reached it, so the leg cannot carry what
	 * the session promised: it ends as a congestion abandon, the rule every
	 * commit caller applies to the same window.
	 */
	CASE("exit: a refused configure behind the retry's stop ends the call");
	fx_reset();
	cfg.srtp_enabled = true;
	calls_hal_features_set(HAL_FEAT_SRTP);
	srtp_uas_up("call-fax-exit-retry-refusal", peer);
	exit_tone_arm(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE);
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE, 900),
		   CALL_FAX_TONE_ENDED);
	rx(uas_in_dialog("INVITE", 2, NULL, NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(call_fax_exit(exit_handle()));
	med.stop_transient = 2;
	med.config_ret = HAL_TRANSIENT;
	rx(uas_in_dialog("ACK", 2, NULL,
			 savp_body(T_PEER, 20000, "0 101",
				   srtp_crypto_line(1, SUITE_EXIT_80, peer2),
				   NULL)));
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.cls, CALL_FAIL_CONGESTION);
	running = true;
	calls_line_fax_kind(0, &running);
	CHECK(!running);
	med.config_ret = HAL_OK;
	cfg_default();
	calls_hal_features_set(0);

	/* the same window through the kind entry, whose push stops the stream
	 * for the rekey the refused ACK left pending */
	CASE("exit: and the same refusal behind a kind change ends it too");
	fx_reset();
	cfg.srtp_enabled = true;
	calls_hal_features_set(HAL_FEAT_SRTP);
	srtp_uas_up("call-fax-kind-retry-refusal", peer);
	med.stop_transient = 2;
	rx(uas_in_dialog("INVITE", 2, NULL, NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));
	rx(uas_in_dialog("ACK", 2, NULL,
			 savp_body(T_PEER, 20000, "0 101",
				   srtp_crypto_line(1, SUITE_EXIT_80, peer2),
				   NULL)));
	CHECK_UINT(ev.failed, 0);
	med.config_ret = HAL_TRANSIENT;
	CHECK(!call_fax_kind_set(0, 0, HAL_PASSTHROUGH_MODEM));
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.cls, CALL_FAIL_CONGESTION);
	running = true;
	calls_line_fax_kind(0, &running);
	CHECK(!running);
	med.config_ret = HAL_OK;
	cfg_default();
	calls_hal_features_set(0);

	/*
	 * A genuine answer tone inside the pending window. The leg's record is
	 * clear and the stream still carries the kind, so a gate that read the
	 * stream alone would answer "already carried" and lose the tone: the
	 * answer of the open exchange would then program voice under a real
	 * modem.
	 */
	CASE("exit: a new answer tone under a pending exit reaches the record");
	fx_reset();
	cfg.channel[0].session_max_expires = 1800;
	uac_up("Session-Expires: 1800;refresher=uac\r\n");
	fake_advance(899000);
	exit_tone_arm(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE);
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_MODEM);
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE, 900),
		   CALL_FAX_TONE_ENDED);
	fake_advance(1000);
	CHECK_UINT(count_send("INVITE"), 2);
	CHECK(call_fax_exit(exit_handle()));
	CHECK_UINT(call_fax_kind_get(0, 0), HAL_PASSTHROUGH_OFF);
	call_fax_tone_note(0, 0, HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE,
			   false, 0);
	CHECK(call_fax_kind_set(0, 0, HAL_PASSTHROUGH_MODEM));
	CHECK_UINT(call_fax_kind_get(0, 0), HAL_PASSTHROUGH_MODEM);

	CASE("exit: and the refresh answer programs that kind, not voice");
	reply(200, NULL, "Session-Expires: 1800;refresher=uac\r\n",
	      sdp_body(T_PEER, 20000, "0", NULL));
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_MODEM);
	cfg_default();

	/* --- the rekey the force must not disturb --- */

	/*
	 * media_force is read in the changed term alone. The mode move and the
	 * rekey read the cache, so a leg that returned to voice still stops and
	 * starts its stream for a new key instead of re-configuring under it.
	 */
	CASE("exit: an SRTP leg returns to voice and still rekeys with a stop");
	fx_reset();
	cfg.srtp_enabled = true;
	calls_hal_features_set(HAL_FEAT_SRTP);
	for (i = 0; i < SDP_SRTP_KEY_LEN; i++) {
		peer[i] = (uint8_t)(0x40 + i);
		peer2[i] = (uint8_t)(0x80 + i);
	}
	srtp_uas_up("call-fax-exit-srtp", peer);
	CHECK_UINT(med.last.srtp.suite, HAL_SRTP_AES_CM_128_SHA1_80);
	exit_tone_arm(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE);
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE, 900),
		   CALL_FAX_TONE_ENDED);
	CHECK(call_fax_exit(exit_handle()));
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_OFF);

	CASE("exit: and the peer's new key still stops, configures and starts");
	med.nseq = 0;
	stops = med.stop_calls;
	rx(uas_in_dialog("INVITE", 2, NULL,
			 savp_body(T_PEER, 20000, "0 101",
				   srtp_crypto_line(1, SUITE_EXIT_80, peer2),
				   NULL)));
	CHECK_UINT(med.stop_calls, stops + 1);
	CHECK(med_seq_index(MED_STOP) == 0);
	CHECK(med_seq_index(MED_CONFIG) == 1);
	CHECK(med_seq_index(MED_START) == 2);
	CHECK(!memcmp(med.last.srtp.key_rx, peer2, SDP_SRTP_KEY_LEN));
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_OFF);
	cfg_default();
	calls_hal_features_set(0);

	/*
	 * The same rule with the exit still pending. The key moves on the
	 * answer of the leg's own upspeed, so the completion and the rekey meet
	 * in one frame: the mode rules read the cache and the force reads
	 * nothing of theirs, so the stream still goes down and comes back up
	 * under the new key.
	 */
	CASE("exit: a pending exit still stops and starts for a new key");
	fx_reset();
	cfg.srtp_enabled = true;
	calls_hal_features_set(HAL_FEAT_SRTP);
	for (i = 0; i < SDP_SRTP_KEY_LEN; i++) {
		peer[i] = (uint8_t)(0x40 + i);
		peer2[i] = (uint8_t)(0x80 + i);
	}
	srtp_uas_up("call-fax-exit-pending-rekey", peer);
	call_fax_tone_note(0, 0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL,
			   false, 0);
	CHECK(call_fax_start(0, 0));
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 900),
		   CALL_FAX_TONE_ENDED);
	CHECK(call_fax_exit(exit_handle()));
	med.nseq = 0;
	reply(200, NULL, NULL,
	      savp_body(T_PEER, 20000, "0 101",
			srtp_crypto_line(1, SUITE_EXIT_80, peer2), NULL));
	CHECK(med_seq_index(MED_STOP) == 0);
	CHECK(med_seq_index(MED_CONFIG) == 1);
	CHECK(med_seq_index(MED_START) == 2);
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_OFF);
	CHECK(!memcmp(med.last.srtp.key_rx, peer2, SDP_SRTP_KEY_LEN));
	cfg_default();
	calls_hal_features_set(0);

	/* --- the refusals --- */

	CASE("exit: a stream that refuses the push keeps every fax field");
	exit_kind_leg_up();
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE, 900),
		   CALL_FAX_TONE_ENDED);
	med.config_ret = HAL_FATAL;
	configs = med.config_calls;
	CHECK(!call_fax_exit(exit_handle()));
	CHECK_UINT(med.config_calls, configs + 1);
	med.config_ret = HAL_OK;
	CHECK_UINT(call_fax_kind_get(0, 0), HAL_PASSTHROUGH_MODEM);
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.fax, "passthrough"));
	CHECK(!strcmp(sv.fax_kind, "modem"));

	/* the disabler is the line's and the description is the stream's: a
	 * backend that refuses one still takes the other */
	CASE("exit: a refused tone disabler release blocks nothing");
	exit_kind_leg_up();
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE, 900),
		   CALL_FAX_TONE_ENDED);
	med.latch_ret = HAL_UNSUPPORTED;
	CHECK(call_fax_exit(exit_handle()));
	CHECK_UINT(med.latch_clears, 1);
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_OFF);
	med.latch_ret = HAL_OK;

	CASE("exit: a leg that never carried a kind answers false");
	fx_reset();
	uac_up_codec("18 101", NULL);
	call_fax_tone_note(0, 0, HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE,
			   false, 0);
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE, 900),
		   CALL_FAX_TONE_ENDED);
	CHECK(!call_fax_exit(exit_handle()));
	CHECK_UINT(med.latch_clears, 0);

	/* --- the handle --- */

	CASE("exit: a handle that names no leg answers false");
	fx_reset();
	CHECK_UINT(call_leg_handle_get(0, 0), 0);
	CHECK(!call_fax_exit(0));
	CHECK_UINT(med.latch_clears, 0);

	CASE("exit: a handle of a leg that ended answers false");
	exit_kind_leg_up();
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE, 900),
		   CALL_FAX_TONE_ENDED);
	handle = exit_handle();
	rx(uac_in_dialog("BYE", "peer-tag", 1, NULL, NULL));
	CHECK_UINT(ev.ended, 1);
	CHECK(!call_fax_exit(handle));
	CHECK_UINT(med.latch_clears, 0);

	/*
	 * The slot itself comes back. A new call takes the physical slot the
	 * old leg left and hears its own answer tone, so the index alone names
	 * two calls: only the generation the handle carries tells them apart,
	 * and the deferral of the first call must not exit the second.
	 */
	CASE("exit: a handle whose slot a new call took answers false");
	CHECK(dial(0, 0, "1002"));
	reply(100, NULL, NULL, NULL);
	reply(200, "peer-tag2", NULL, sdp_body(T_PEER, 20004, "0 101", NULL));
	exit_tone_arm(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE);
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE, 900),
		   CALL_FAX_TONE_ENDED);
	CHECK(handle != exit_handle());
	CHECK(!call_fax_exit(handle));
	CHECK_UINT(med.latch_clears, 0);
	CHECK_UINT(call_fax_kind_get(0, 0), HAL_PASSTHROUGH_MODEM);

	/*
	 * The UI name moves as well. A transfer promotes the child onto the
	 * name the parent held, so the slot a deferral was armed under names
	 * the other physical leg by the time it expires. The handle still names
	 * the parent, which is the leg that heard the tone.
	 */
	CASE("exit: a deferral keeps its leg across a transfer promotion");
	refer_up("<sip:1003@" T_DOMAIN ">");
	exit_tone_arm(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE);
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE, 900),
		   CALL_FAX_TONE_ENDED);
	handle = exit_handle();
	child_answer();
	CHECK_UINT(call_slot_stream(0, 0), 1);
	CHECK(exit_handle() != handle);
	CHECK_UINT(call_fax_kind_get(0, 0), HAL_PASSTHROUGH_OFF);
	/* the swap stopped the parent's stream, and this push must leave it
	 * stopped: it stopped nothing of its own */
	starts = med.start_calls;
	CHECK(call_fax_exit(handle));
	CHECK_UINT(med.start_calls, starts);
	CHECK_UINT(med.latch_clears, 1);

	CASE("exit: and the leg that took the name carries no fax of its own");
	CHECK_UINT(call_fax_kind_get(0, 0), HAL_PASSTHROUGH_OFF);
	CHECK(!call_fax_on(0, 0));

	/* --- the leg that converts a second time --- */

	CASE("exit: a genuine tone behind the exit runs a second upspeed");
	fx_reset();
	uac_up_codec("18 101", NULL);
	call_fax_tone_note(0, 0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL,
			   false, 0);
	CHECK(call_fax_start(0, 0));
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "0 101", NULL));
	CHECK_UINT(exit_tone_end(HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 900),
		   CALL_FAX_TONE_ENDED);
	CHECK(call_fax_exit(exit_handle()));
	CHECK(!call_fax_on(0, 0));
	call_fax_tone_note(0, 0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL,
			   false, 0);
	CHECK(call_fax_start(0, 0));
	CHECK_UINT(med.fax_calls, 2);
	CHECK(call_fax_on(0, 0));
	CHECK_UINT(med.latch_clears, 1);
}

static void test_line_fax_kind(void)
{
	struct req_info ri;
	int ridx;

	CASE("line kind: a line with no leg at all names nothing");
	fx_reset();
	CHECK_UINT(calls_line_fax_kind(0, NULL), HAL_PASSTHROUGH_OFF);

	CASE("line kind: a leg with no running stream names nothing");
	CHECK(dial(0, 0, "1002"));
	reply(180, "peer-tag", NULL, NULL);
	CHECK_UINT(calls_line_fax_kind(0, NULL), HAL_PASSTHROUGH_OFF);

	CASE("line kind: a plain voice call names nothing either");
	fx_reset();
	uac_up(NULL);
	CHECK_UINT(calls_line_fax_kind(0, NULL), HAL_PASSTHROUGH_OFF);

	CASE("line kind: the answer tone puts the modem kind on the line");
	CHECK(call_fax_start(0, 0));
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "0 101", NULL));
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_MODEM);
	CHECK_UINT(calls_line_fax_kind(0, NULL), HAL_PASSTHROUGH_MODEM);

	CASE("line kind: the preamble moves the line to the fax kind");
	CHECK(call_fax_kind_set(0, 0, HAL_PASSTHROUGH_FAX));
	CHECK_UINT(calls_line_fax_kind(0, NULL), HAL_PASSTHROUGH_FAX);

	/*
	 * The hold gate. A held leg puts nothing of its own on the wire, so the
	 * library stops counting it and hands the canceller back to whatever
	 * else the line is carrying. This is the case the gate exists for: a
	 * held fax leg must stop naming the line's policy.
	 */
	CASE("line kind: holding the fax leg takes its kind off the line");
	CHECK(call_hold(0, 0));
	CHECK(last_req(&ri));
	ridx = find_send("INVITE");
	reply_at((unsigned int)ridx, 200, NULL, NULL,
		 sdp_body(T_PEER, 20000, "0", "a=recvonly\r\n"));
	CHECK(med.last.tx_silence);
	CHECK_UINT(calls_line_fax_kind(0, NULL), HAL_PASSTHROUGH_OFF);

	CASE("line kind: and resuming it puts the kind back");
	CHECK(call_resume(0, 0));
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "0 101", NULL));
	CHECK(!med.last.tx_silence);
	CHECK_UINT(calls_line_fax_kind(0, NULL), HAL_PASSTHROUGH_FAX);

	/*
	 * One canceller serves the line, so two legs carrying different kinds
	 * still resolve to one policy. The modem kind wins: it takes the
	 * canceller off the frame whole, which is the safe answer for a carrier
	 * of either kind, and the fax policy only relaxes it. The fax leg is
	 * the one the walk reaches first, so a walk that stopped at the first
	 * kind it found would answer fax here.
	 */
	CASE("line kind: a second leg on the line takes the modem kind");
	fx_reset();
	uas_up("kind-s0");
	rx(invite_text("kind-s1", NULL, sdp_body(T_PEER, 20002, "0 101", NULL)));
	CHECK_UINT(ev.slot, 1);
	CHECK(call_ring(0, 1));
	CHECK(call_answer(0, 1));
	rx(uas_in_dialog("ACK", 1, NULL, NULL));
	CHECK(call_fax_start(0, 1));
	reply(200, NULL, NULL, sdp_body(T_PEER, 20002, "0 101", NULL));
	CHECK_UINT(calls_line_fax_kind(0, NULL), HAL_PASSTHROUGH_MODEM);

	CASE("line kind: and it still wins once the first leg reaches the fax kind");
	CHECK(call_fax_start(0, 0));
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "0 101", NULL));
	CHECK(call_fax_kind_set(0, 0, HAL_PASSTHROUGH_FAX));
	CHECK_UINT(calls_line_fax_kind(0, NULL), HAL_PASSTHROUGH_MODEM);

	CASE("line kind: holding the modem leg leaves the fax kind standing");
	CHECK(call_hold(0, 1));
	ridx = find_send("INVITE");
	reply_at((unsigned int)ridx, 200, NULL, NULL,
		 sdp_body(T_PEER, 20002, "0", "a=recvonly\r\n"));
	CHECK_UINT(calls_line_fax_kind(0, NULL), HAL_PASSTHROUGH_FAX);

	CASE("line kind: a line this board does not carry names nothing");
	CHECK_UINT(calls_line_fax_kind(CFG_CHANNEL_MAX, NULL), HAL_PASSTHROUGH_OFF);

	/*
	 * The library reads a T.38 leg as the modem kind whatever its
	 * description says (udsp media_passthrough_kind): the image session is
	 * full duplex data, so the canceller is off the frame whole. A walk
	 * that read the description alone would answer voice here and the row
	 * would name a policy the DSP is not running.
	 */
	CASE("line kind: a T.38 leg reads as the modem kind");
	fx_reset();
	cfg.channel[0].fax_t38_enable = true;
	calls_hal_features_set(HAL_FEAT_T38);
	uac_up_codec("18 101", NULL);
	CHECK(call_fax_start(0, 0));
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "0 101", NULL));
	CHECK(call_fax_kind_set(0, 0, HAL_PASSTHROUGH_FAX));
	CHECK_UINT(calls_line_fax_kind(0, NULL), HAL_PASSTHROUGH_FAX);
	CHECK_UINT(call_t38_start(0, 0), CALL_T38_STARTED);
	reply(200, NULL, NULL, t38_body(T_PEER, 20000, NULL, NULL));
	CHECK_UINT(med.last.codec, HAL_CODEC_T38);
	CHECK_UINT(calls_line_fax_kind(0, NULL), HAL_PASSTHROUGH_MODEM);

	/* The out parameter says whether ANY leg carries running media, held
	 * legs included: the caller trusts a latch view only while the DSP can
	 * still hold one (its latch dies with the line's streams). */
	CASE("line kind: media_running follows the line's streams");
	{
		bool running = true;

		fx_reset();
		CHECK_UINT(calls_line_fax_kind(0, &running), HAL_PASSTHROUGH_OFF);
		CHECK(!running);
		uac_up(NULL);
		CHECK_UINT(calls_line_fax_kind(0, &running), HAL_PASSTHROUGH_OFF);
		CHECK(running);
		CHECK(call_hold(0, 0));
		reply(200, NULL, NULL,
		      sdp_body(T_PEER, 20000, "0", "a=recvonly\r\n"));
		CHECK(med.last.tx_silence);
		CHECK_UINT(calls_line_fax_kind(0, &running), HAL_PASSTHROUGH_OFF);
		CHECK(running);
	}
}

/*
 * The call record report. Two properties are asserted on every ending path
 * this layer has: the report fires exactly once for a leg that was in use,
 * and the media snapshot stands in front of the stream stop. The second is
 * the whole reason the snapshot lives in media_stop(): libudsp answers the
 * stats query with UDSP_UNSUPPORTED once the stop makes the stream idle.
 */
/* ---------------------------------------------------------- the T.38 upgrade */

/*
 * The two gates the leg reads at its start: the channel option and the
 * backend feature bit. Both are read once, when the leg allocates its media,
 * so a case sets them before it dials.
 */
static void t38_enable(bool on)
{
	unsigned int i;

	for (i = 0; i < CFG_CHANNEL_MAX; i++)
		cfg.channel[i].fax_t38_enable = on;

	calls_hal_features_set(on ? HAL_FEAT_T38 : 0);
}

/*
 * One image section with the RFC 3362 attributes a peer sends, either as an
 * offer or as the answer to ours. The numbers are deliberately not our own
 * defaults, so a result that carries them proves the reconciliation ran:
 * 9600 is below our 14400 ceiling and wins, and 400 is a datagram size we would
 * never have named.
 *
 * audio, when it is given, is a whole m=audio line. An upgrade offer withdraws
 * the audio stream by naming it at port 0, and an image-only offer names none.
 */
static const char *t38_body(const char *addr, unsigned int port, const char *audio,
			    const char *extra)
{
	char *buf = text_slot();

	snprintf(buf, TEXT_MAX,
		 "v=0\r\n"
		 "o=- 42 42 IN IP4 %s\r\n"
		 "s=-\r\n"
		 "c=IN IP4 %s\r\n"
		 "t=0 0\r\n"
		 "%s"
		 "m=image %u udptl t38\r\n"
		 "a=T38FaxVersion:0\r\n"
		 "a=T38FaxRateManagement:transferredTCF\r\n"
		 "a=T38MaxBitRate:9600\r\n"
		 "a=T38FaxMaxDatagram:400\r\n"
		 "a=T38FaxUdpEC:t38UDPRedundancy\r\n"
		 "%s",
		 addr, addr, audio ? audio : "", port, extra ? extra : "");

	return buf;
}

/* a G.729 call with T.38 enabled on both gates, which is where every case
 * below starts: G.729 is the codec a fax cannot cross */
static void t38_call_up(void)
{
	fx_reset();
	t38_enable(true);
	uac_up_codec("18 101", NULL);
}

/*
 * The same call with the G.711 passthrough of the answer tone on it and the
 * upspeed answered, which is the leg every upgrade below starts from: the
 * image session replaces a carrier this side already runs, and the entry
 * refuses a leg without one.
 */
static void t38_passthrough_up(void)
{
	t38_call_up();
	call_fax_start(0, 0);
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "0 101", NULL));
}

/*
 * Two lines, both in that passthrough, which the daemon-wide admission cases
 * need: the line that loses the one image session has to be refused for the
 * session and not for a carrier it never took.
 */
static void t38_two_passthrough_up(void)
{
	t38_passthrough_up();
	dial(1, 0, "1002");
	reply(100, NULL, NULL, NULL);
	reply(200, "peer-tag2", NULL, sdp_body(T_PEER, 20002, "0 101", NULL));
	call_fax_start(1, 0);
	reply(200, NULL, NULL, sdp_body(T_PEER, 20002, "0 101", NULL));
}

/* the same call, already upgraded and committed to an image session */
static void t38_session_up(void)
{
	t38_passthrough_up();
	call_t38_start(0, 0);
	reply(200, NULL, NULL, t38_body(T_PEER, 30000, NULL, NULL));
}

/* what a leg offers next, which is the only way a test can read the leg's own
 * T.38 enable back: the flag has no accessor and sdp_offer_build() reads it */
static bool next_offer_is_image(void)
{
	struct req_info ri;

	if (!call_hold(0, 0) || !last_req(&ri))
		return false;

	return has(ri.body, "m=image");
}

/*
 * The T.38 upgrade end to end; the trigger belongs to line/line.c. This group
 * covers everything that the leg does with it: the image offer, and the stream
 * transition that the answer drives. It also covers the refusal that leaves
 * the leg on its G.711 passthrough, and the inbound accept behind the
 * one-session admission. Last, it covers the flag lifecycle that holds all of
 * it together.
 *
 * Every upgrade below starts from a passthrough leg, because the entry takes
 * an upgrade from no other leg. The answer tone converts the stream, and the
 * image session replaces that carrier.
 */
static void test_t38(void)
{
	struct req_info ri, ri2;
	struct sdp_result res;
	struct cfg_channel ch;
	struct hal_media m;
	struct slot_view sv;
	unsigned int before, snaps, configs, faxes, starts;
	char first[1200];		/* the widest body req_info_at() keeps */
	const char *a, *b;

	/* 1. the outbound upgrade */
	t38_passthrough_up();

	CASE("t38: the leg runs the G.711 passthrough of the answer tone before "
	     "the upgrade");
	CHECK_UINT(med.last.codec, HAL_CODEC_PCMU);
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.fax, "passthrough"));
	before = fk.sends;
	configs = med.stop_calls;

	/* the dial and the upspeed of the answer tone are behind it, so this is
	 * the third INVITE of the leg and the third CSeq */
	CASE("t38: the upgrade puts one re-INVITE on the wire");
	CHECK(call_t38_start(0, 0) == CALL_T38_STARTED);
	CHECK_UINT(fk.sends, before + 1);
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.method, "INVITE"));
	CHECK_UINT(ri.cseq, 3);

	CASE("t38: whose offer REPLACES the audio section with an image one");
	CHECK(has(ri.body, "m=image 10000 udptl t38\r\n"));
	CHECK(!has(ri.body, "m=audio"));
	CHECK(has(ri.body, "a=T38FaxRateManagement:transferredTCF"));
	CHECK(!has(ri.body, "a=ptime"));

	CASE("t38: and it lists the timer option tag without 100rel (RFC 4028 7.1)");
	CHECK(has(last_text(), "Supported: " UA_SUPPORTED_REINVITE "\r\n"));
	CHECK(!has(last_text(), "100rel"));

	CASE("t38: and nothing has moved on the stream yet");
	CHECK_UINT(med.stop_calls, configs);
	CHECK_UINT(med.last.codec, HAL_CODEC_PCMU);

	snaps = med.snapshot_calls;
	med.nseq = 0;
	reply(200, NULL, NULL, t38_body(T_PEER, 30000, NULL, NULL));

	CASE("t38: the answer stops the audio stream, configures, then starts");
	CHECK_UINT(med.stop_calls, configs + 1);
	CHECK(med_seq_index(MED_STOP) == 0);
	CHECK(med_seq_index(MED_CONFIG) == 1);
	CHECK(med_seq_index(MED_START) == 2);

	/* one snapshot per record, and the leg's own is at teardown. A
	 * transition that took a second one would either overwrite the
	 * record's numbers or spend the stream's one job slot. */
	CASE("t38: and takes NO media snapshot on the way through");
	CHECK_UINT(med.snapshot_calls, snaps);

	CASE("t38: the stream carries the negotiated image description");
	CHECK_UINT(med.last.codec, HAL_CODEC_T38);
	CHECK_UINT(med.last.t38.max_bit_rate, 9600);
	CHECK_UINT(med.last.t38.max_datagram, 400);
	CHECK_UINT(med.last.t38.ec, HAL_T38_EC_REDUNDANCY);
	CHECK_UINT(med.last.direction, HAL_MEDIA_SENDRECV);
	CHECK_UINT(med.last.remote.port, 30000);

	/* the dial, the upspeed and the upgrade */
	CASE("t38: the exchange is acknowledged and reports no feature outcome");
	CHECK_UINT(count_send("ACK"), 3);
	CHECK_UINT(ev.reinvite_done, 0);
	CHECK_UINT(ev.failed, 0);

	CASE("t38: the status dump names the codec, the ptime and the fax mode");
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.codec, "t38"));
	CHECK(sv.has_ptime);
	CHECK_UINT(sv.ptime, 0);
	CHECK(!strcmp(sv.fax, "t38"));

	CASE("t38: the record of that leg says it carried T.38");
	CHECK(call_hangup(0, 0));
	reply(200, NULL, NULL, NULL);
	fake_advance(6000);
	CHECK_UINT(ev.ended, 1);
	CHECK(ev.end.t38);
	CHECK_UINT(ev.end.codec, HAL_CODEC_NONE);

	/* 2. the media fill, driven directly */
	CASE("t38: the fill turns a T.38 result into the one mode key");
	memset(&ch, 0, sizeof(ch));
	ch.ptime_ms = 20;
	ch.jitter_target = 50;
	ch.jitter_max = 100;
	memset(&res, 0, sizeof(res));
	res.t38 = true;
	res.t38_max_bit_rate = 9600;
	res.t38_max_datagram = 400;
	res.t38_ec = SDP_T38_EC_REDUNDANCY;
	res.dir = SDP_SENDRECV;
	res.valid = true;
	snprintf(res.remote_addr, sizeof(res.remote_addr), "%s", T_PEER);
	res.remote_port = 30000;
	media_fill(&ch, false, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.codec, HAL_CODEC_T38);
	CHECK_UINT(m.t38.max_bit_rate, 9600);
	CHECK_UINT(m.t38.max_datagram, 400);
	CHECK_UINT(m.t38.ec, HAL_T38_EC_REDUNDANCY);

	CASE("t38: a peer that named no error correction programs none");
	res.t38_ec = SDP_T38_EC_NONE;
	media_fill(&ch, false, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.t38.ec, HAL_T38_EC_NONE);

	CASE("t38: an audio result never sets the T.38 mode");
	memset(&res, 0, sizeof(res));
	res.codec.id = SDP_CODEC_PCMU;
	res.dtmf_pt = -1;
	res.cn_pt = -1;
	res.valid = true;
	snprintf(res.remote_addr, sizeof(res.remote_addr), "%s", T_PEER);
	res.remote_port = 30000;
	media_fill(&ch, false, 0, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.codec, HAL_CODEC_PCMU);
	CHECK_UINT(m.t38.max_bit_rate, 0);

	/*
	 * 3. The upgrade of a passthrough leg, which is the only leg
	 * line/line.c starts one from: the local CED arms the G.711 passthrough
	 * and the V.21 preamble upgrades it (line/line.c, the two rules of the
	 * second stage). The upspeed of that answer tone is normally still open
	 * when the preamble lands, and the leg says so with an answer of its
	 * own, because the preamble repeats until the exchange closes.
	 */
	t38_call_up();
	CHECK(call_fax_start(0, 0));

	CASE("t38: the upgrade refuses for state while the upspeed is open and does "
	     "not spend anything");
	before = fk.sends;
	CHECK(call_t38_start(0, 0) == CALL_T38_BUSY);
	CHECK_UINT(fk.sends, before);
	CHECK_UINT(ev.failed, 0);

	CASE("t38: the upgrade starts after the upspeed answer on a passthrough leg");
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "0 101", NULL));
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.fax, "passthrough"));
	/* the preamble that starts the upgrade also names the kind */
	CHECK(call_fax_kind_set(0, 0, HAL_PASSTHROUGH_FAX));
	CHECK(call_t38_start(0, 0) == CALL_T38_STARTED);
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.method, "INVITE"));
	CHECK(has(ri.body, "m=image 10000 udptl t38\r\n"));
	CHECK(!has(ri.body, "m=audio"));

	/* a refused image offer, with nothing left to convert:
	 * the carrier the refusal falls back on is the one this leg came with */
	CASE("t38: a refused upgrade makes no second switch on a leg already in "
	     "passthrough");
	faxes = med.fax_calls;
	reply(488, NULL, NULL, NULL);
	CHECK_UINT(med.fax_calls, faxes);
	CHECK_UINT(med.last.codec, HAL_CODEC_PCMU);
	CHECK_UINT(med.last.jb_mode, HAL_JB_FIXED);

	/* the dial, the upspeed and the image offer, and the ACK of the refusal
	 * behind them: no offer of any kind follows the 488 */
	CASE("t38: and puts no further INVITE on the wire");
	CHECK_UINT(count_send("INVITE"), 3);
	CHECK_UINT(ev.failed, 0);
	CHECK_UINT(ev.reinvite_done, 0);

	/*
	 * The image session the peer refused leaves the leg on the audio one it
	 * already carries, and that stream is running a fax: the kind has to
	 * stand, and the next exchange of the leg has to carry it too.
	 */
	CASE("fax: a refused T.38 upgrade keeps the fax kind");
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_FAX);
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.fax, "passthrough"));
	CHECK(!strcmp(sv.fax_kind, "fax"));
	CHECK(call_hold(0, 0));
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "0", "a=recvonly\r\n"));
	CHECK_UINT(med.last.passthrough, HAL_PASSTHROUGH_FAX);

	/* 4. the 491 glare retry re-offers the same image section */
	t38_passthrough_up();
	CHECK(call_t38_start(0, 0) == CALL_T38_STARTED);
	CHECK(last_req(&ri));
	snprintf(first, sizeof(first), "%s", ri.body);
	reply(491, NULL, NULL, NULL);

	CASE("t38: a 491 puts no immediate retry on the wire");
	CHECK_UINT(count_send("INVITE"), 3);

	CASE("t38: the armed retry re-offers the image section byte for byte");
	fake_advance(4000);
	CHECK(last_req(&ri2));
	CHECK(!strcmp(ri2.method, "INVITE"));
	CHECK_UINT(ri2.cseq, 4);
	a = strstr(first, "m=image");
	b = strstr(ri2.body, "m=image");
	CHECK(a != NULL);
	CHECK(b != NULL);
	CHECK(a && b && !strcmp(a, b));

	/* RFC 3264 8: the o= version moves with every offer this side emits, so
	 * the whole body is not identical and must not be asserted as such */
	CASE("t38: on a session description version of its own");
	CHECK(strcmp(first, ri2.body) != 0);

	CASE("t38: and the answered retry still commits the image session");
	reply(200, NULL, NULL, t38_body(T_PEER, 30000, NULL, NULL));
	CHECK_UINT(med.last.codec, HAL_CODEC_T38);

	/* 5. an authentication challenge keeps the image offer */
	t38_passthrough_up();
	CHECK(call_t38_start(0, 0) == CALL_T38_STARTED);
	reply(401, NULL, "WWW-Authenticate: Digest realm=\"" T_DOMAIN "\", nonce=\""
	      T_NONCE "\"\r\n", NULL);

	CASE("t38: a challenged upgrade resends with credentials and keeps the image");
	CHECK_UINT(count_send("INVITE"), 4);
	CHECK(has(last_text(), "Authorization: Digest"));
	CHECK(last_req(&ri));
	CHECK(has(ri.body, "m=image "));
	CHECK(!has(ri.body, "m=audio"));

	/* 6. the inbound accept */
	fx_reset();
	t38_enable(true);
	uac_up(NULL);
	snaps = med.snapshot_calls;
	med.nseq = 0;
	rx(uac_in_dialog("INVITE", "peer-tag", 2, NULL,
			 t38_body(T_PEER, 30000, "m=audio 0 RTP/AVP 0\r\n", NULL)));

	CASE("t38: an inbound upgrade is answered 200 in the offered section order");
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(has(last_text(), "m=audio 0 RTP/AVP 0\r\n"));
	CHECK(has(last_text(), "m=image 10000 udptl t38\r\n"));

	CASE("t38: the stream transition runs at the 200 and takes no snapshot");
	CHECK_UINT(med.last.codec, HAL_CODEC_T38);
	CHECK(med_seq_index(MED_STOP) == 0);
	CHECK(med_seq_index(MED_CONFIG) == 1);
	CHECK_UINT(med.snapshot_calls, snaps);

	CASE("t38: the negotiation is still the committed AUDIO one before the ACK");
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.codec, "PCMU"));

	CASE("t38: and the ACK commits it");
	rx(uac_in_dialog("ACK", "peer-tag", 2, NULL, NULL));
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.codec, "t38"));
	CHECK(!strcmp(sv.fax, "t38"));

	CASE("t38: a refresh of ours then re-offers the image session");
	CHECK(call_hold(0, 0));
	CHECK(last_req(&ri));
	CHECK(has(ri.body, "m=image "));
	CHECK(!has(ri.body, "a=sendonly"));
	reply(200, NULL, NULL, t38_body(T_PEER, 30000, NULL, NULL));

	CASE("t38: an image-only offer is accepted the same way");
	fx_reset();
	t38_enable(true);
	uac_up(NULL);
	rx(uac_in_dialog("INVITE", "peer-tag", 2, NULL, t38_body(T_PEER, 30000, NULL, NULL)));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(has(last_text(), "m=image 10000 udptl t38\r\n"));
	CHECK(!has(last_text(), "m=audio"));
	CHECK_UINT(med.last.codec, HAL_CODEC_T38);
	rx(uac_in_dialog("ACK", "peer-tag", 2, NULL, NULL));

	/*
	 * 7. The session interactions of a committed image session. No image
	 * section carries a direction attribute, so a hold re-INVITE is an
	 * identical image offer and the hold state never reaches the far end;
	 * an offerless re-INVITE is answered with the same offer in the 2xx,
	 * and the ACK carries the answer.
	 */
	t38_session_up();
	/* the upgrade INVITE, not the ACK the answer drew behind it */
	CHECK(req_of("INVITE", &ri));
	snprintf(first, sizeof(first), "%s", ri.body);

	CASE("t38: a hold made during T.38 re-offers the same image section");
	CHECK(call_hold(0, 0));
	CHECK(last_req(&ri2));
	a = strstr(first, "m=image");
	b = strstr(ri2.body, "m=image");
	CHECK(a != NULL);
	CHECK(b != NULL);
	CHECK(a && b && !strcmp(a, b));

	CASE("t38: with no hold attribute anywhere in it");
	CHECK(!has(ri2.body, "a=sendonly"));
	CHECK(!has(ri2.body, "a=inactive"));
	reply(200, NULL, NULL, t38_body(T_PEER, 30000, NULL, NULL));
	CHECK_UINT(ev.reinvite_done, 1);
	CHECK(ev.reinvite_ok);

	CASE("t38: an offerless re-INVITE during T.38 offers the image in the 2xx");
	t38_session_up();
	rx(uac_in_dialog("INVITE", "peer-tag", 2, NULL, NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(has(last_text(), "m=image "));
	CHECK(!has(last_text(), "m=audio"));

	CASE("t38: and the answer in the ACK commits it");
	rx(uac_in_dialog("ACK", "peer-tag", 2, NULL, t38_body(T_PEER, 30004, NULL, NULL)));
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.codec, "t38"));
	CHECK_UINT(med.last.codec, HAL_CODEC_T38);
	CHECK_UINT(med.last.remote.port, 30004);
	CHECK_UINT(ev.failed, 0);

	/* 8. the peer takes the session back to audio */
	CASE("t38: a peer offer that returns to audio moves the stream back");
	med.nseq = 0;
	rx(uac_in_dialog("INVITE", "peer-tag", 3, NULL, sdp_body(T_PEER, 20000, "0 101", NULL)));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(med.last.codec, HAL_CODEC_PCMU);
	CHECK(med_seq_index(MED_STOP) == 0);
	CHECK(med_seq_index(MED_CONFIG) == 1);

	CASE("t38: the commit releases the T.38 session, so the next offer is audio");
	rx(uac_in_dialog("ACK", "peer-tag", 3, NULL, NULL));
	CHECK(!next_offer_is_image());
	CHECK(last_req(&ri));
	CHECK(has(ri.body, "m=audio "));
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "0", "a=recvonly\r\n"));

	/* the image session is gone and the carrier under it is not: the leg
	 * went back to the G.711 passthrough the answer tone applied */
	CASE("t38: and the status dump names that passthrough again, not T.38");
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.codec, "PCMU"));
	CHECK(!strcmp(sv.fax, "passthrough"));

	/* 9. the session refresh of a committed image session */
	fx_reset();
	t38_enable(true);
	cfg.channel[0].session_max_expires = 1800;
	call_dial(0, 0, "1002", true);
	reply(100, NULL, NULL, NULL);
	reply(200, "peer-tag", "Session-Expires: 1800;refresher=uac\r\n",
	      sdp_body(T_PEER, 20000, "18 101", NULL));
	CHECK(call_fax_start(0, 0));
	reply(200, NULL, "Session-Expires: 1800;refresher=uac\r\n",
	      sdp_body(T_PEER, 20000, "0 101", NULL));
	CHECK(call_t38_start(0, 0) == CALL_T38_STARTED);
	reply(200, NULL, "Session-Expires: 1800;refresher=uac\r\n",
	      t38_body(T_PEER, 30000, NULL, NULL));

	CASE("t38: the session refresh re-offers the image session");
	fake_advance(900000);
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.method, "INVITE"));
	CHECK_UINT(ri.cseq, 4);
	CHECK(has(ri.body, "m=image "));
	CHECK(!has(ri.body, "m=audio"));
	cfg.channel[0].session_max_expires = 0;

	/* 10. the transition stop, whose status the leg reads */
	t38_passthrough_up();
	CHECK(call_t38_start(0, 0) == CALL_T38_STARTED);
	med.stop_transient = 1;
	configs = med.config_calls;
	before = med.stop_calls;
	reply(200, NULL, NULL, t38_body(T_PEER, 30000, NULL, NULL));

	CASE("t38: a transient stop is retried once and the transition goes on");
	CHECK_UINT(med.stop_calls, before + 2);
	CHECK_UINT(med.config_calls, configs + 1);
	CHECK_UINT(med.last.codec, HAL_CODEC_T38);
	CHECK_UINT(ev.failed, 0);

	t38_passthrough_up();
	CHECK(call_t38_start(0, 0) == CALL_T38_STARTED);
	med.stop_transient = 1;
	med.stop_ret = HAL_FATAL;
	configs = med.config_calls;
	snaps = med.snapshot_calls;
	before = med.stop_calls;
	reply(200, NULL, NULL, t38_body(T_PEER, 30000, NULL, NULL));

	/* two stops in the transition, then the one the teardown makes. The
	 * snapshot count is what tells them apart: the transition takes none
	 * and the teardown takes exactly one. */
	CASE("t38: a stop that fails after the retry configures nothing");
	CHECK_UINT(med.stop_calls, before + 3);
	CHECK_UINT(med.snapshot_calls, snaps + 1);
	CHECK_UINT(med.config_calls, configs);

	CASE("t38: and the leg that cannot leave audio is abandoned as congestion");
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.cls, CALL_FAIL_CONGESTION);
	CHECK(find_send("BYE") > 0);
	med.stop_ret = HAL_OK;

	/*
	 * The other half of the transition window: the stop succeeded and the
	 * configuration behind it was refused, so the stream is down in neither
	 * mode and the call ends the same way. The record of that leg carries
	 * no media table: the teardown stop finds a stream this leg already
	 * stopped, so it takes no snapshot at all, and an absent table is
	 * preferred to numbers from the phase before the transition.
	 */
	t38_passthrough_up();
	CHECK(call_t38_start(0, 0) == CALL_T38_STARTED);
	med.config_ret = HAL_FATAL;
	med.nseq = 0;
	snaps = med.snapshot_calls;
	starts = med.start_calls;
	before = med.stop_calls;
	reply(200, NULL, NULL, t38_body(T_PEER, 30000, NULL, NULL));
	med.config_ret = HAL_OK;

	CASE("t38: a config refused after the transition stop starts nothing");
	CHECK(med_seq_index(MED_STOP) == 0);
	CHECK(med_seq_index(MED_CONFIG) == 1);
	CHECK_UINT(med.start_calls, starts);

	CASE("t38: the leg is abandoned as congestion");
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.cls, CALL_FAIL_CONGESTION);
	CHECK(find_send("BYE") > 0);

	CASE("t38: and its record takes NO media snapshot at all");
	CHECK_UINT(med.snapshot_calls, snaps);
	CHECK(med_seq_index(MED_SNAPSHOT) < 0);
	CHECK_UINT(med.stop_calls, before + 1);

	/* 11. the one-session admission, daemon wide. The second line takes the
	 * passthrough of its own answer tone, so the admission is the gate it
	 * reads and not the carrier the upgrade needs. */
	t38_session_up();
	CHECK(dial(1, 0, "1002"));
	reply(100, NULL, NULL, NULL);
	reply(200, "peer-tag2", NULL, sdp_body(T_PEER, 20002, "0 101", NULL));
	CHECK(call_fax_start(1, 0));
	reply(200, NULL, NULL, sdp_body(T_PEER, 20002, "0 101", NULL));
	before = fk.sends;

	CASE("t38: the second line is refused while the first holds the session");
	CHECK(call_t38_start(1, 0) == CALL_T38_REFUSED);
	CHECK_UINT(fk.sends, before);

	/*
	 * The admission answers exactly as a disabled leg does, which is what
	 * the loc copy buys: an upgrade offer that withdrew the audio stream
	 * leaves nothing to answer, so it is refused 488, and an offer that
	 * kept its audio is answered with the image section at port 0 and the
	 * audio session standing. The 488 is the one that proves the admission,
	 * because the same offer is accepted on this leg once the first one
	 * hands the session back, three cases below.
	 */
	CASE("t38: an upgrade offer to that line is refused while the session is taken");
	rx(uac_in_dialog("INVITE", "peer-tag2", 2, NULL,
			 t38_body(T_PEER, 30002, "m=audio 0 RTP/AVP 0\r\n", NULL)));
	CHECK(has(last_text(), "SIP/2.0 488"));

	CASE("t38: and one that keeps its audio is answered with image port 0");
	rx(uac_in_dialog("INVITE", "peer-tag2", 3, NULL,
			 t38_body(T_PEER, 30002, "m=audio 20002 RTP/AVP 0\r\n", NULL)));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(has(last_text(), "m=image 0 udptl t38\r\n"));

	CASE("t38: which leaves that leg its audio and no T.38 state");
	rx(uac_in_dialog("ACK", "peer-tag2", 3, NULL, NULL));
	CHECK(slot_view_get(1, 0, &sv));
	CHECK(!strcmp(sv.codec, "PCMU"));
	CHECK(!strcmp(sv.fax, "passthrough"));

	CASE("t38: the released first leg hands the session back");
	CHECK(call_hangup(0, 0));
	reply(200, NULL, NULL, NULL);
	fake_advance(6000);
	CHECK(ev.end.t38);

	CASE("t38: so the offer that was refused a moment ago is now accepted");
	rx(uac_in_dialog("INVITE", "peer-tag2", 4, NULL,
			 t38_body(T_PEER, 30002, "m=audio 0 RTP/AVP 0\r\n", NULL)));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(has(last_text(), "m=image 10002 udptl t38\r\n"));
	rx(uac_in_dialog("ACK", "peer-tag2", 4, NULL, NULL));
	CHECK(slot_view_get(1, 0, &sv));
	CHECK(!strcmp(sv.fax, "t38"));

	/*
	 * 12. The answer tone is ignored once the session is an image one. The
	 * two latches are
	 * independent, so a CED that arrives after the upgrade reaches the
	 * leg; it must not push a G.711 profile onto a UDPTL stream, and it
	 * must not re-offer the image session as a passthrough exchange.
	 */
	CASE("t38: a CED after the committed upgrade starts no passthrough");
	t38_session_up();
	faxes = med.fax_calls;
	before = fk.sends;
	CHECK(!call_fax_start(0, 0));
	CHECK_UINT(med.fax_calls, faxes);
	CHECK_UINT(fk.sends, before);

	CASE("t38: and the image session is untouched by it");
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.codec, "t38"));
	CHECK(!strcmp(sv.fax, "t38"));
	CHECK_UINT(med.last.codec, HAL_CODEC_T38);

	CASE("t38: while a leg with an audio session still takes that tone");
	t38_call_up();
	CHECK(call_fax_start(0, 0));
	CHECK_UINT(med.fax_calls, 1);

	/*
	 * 13. The glare retry re-competes for the one session. The 491 released
	 * it, so the second line can take it while the retry timer runs; the
	 * retry then loses the race and must keep the G.711 passthrough the leg
	 * already runs instead of admitting a second image session behind the
	 * first.
	 */
	CASE("t38: a 491 releases the session, so the other line can take it");
	t38_two_passthrough_up();
	CHECK(call_t38_start(0, 0) == CALL_T38_STARTED);
	reply(491, NULL, NULL, NULL);
	CHECK(call_t38_start(1, 0) == CALL_T38_STARTED);
	before = fk.sends - 1;			/* the second line's image offer */

	/* the profile goes down in front of every upspeed offer
	 * (fax_upspeed_run), so a fax count that did not move says no second
	 * conversion and no offer behind one */
	CASE("t38: and the retry that lost the race keeps the passthrough it runs");
	faxes = med.fax_calls;
	fake_advance(4000);
	CHECK_UINT(med.fax_calls, faxes);
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.fax, "passthrough"));

	CASE("t38: and the leg is back on audio with its image enable restored");
	CHECK(!next_offer_is_image());

	CASE("t38: while the line that won the race commits its image session");
	reply_at(before, 200, NULL, NULL, t38_body(T_PEER, 30002, NULL, NULL));
	CHECK(slot_view_get(1, 0, &sv));
	CHECK(!strcmp(sv.codec, "t38"));
	CHECK(!strcmp(sv.fax, "t38"));
	CHECK_UINT(ev.failed, 0);

	/* 14. the gates of the entry itself */
	CASE("t38: a slot with no leg refuses the upgrade");
	t38_call_up();
	CHECK(call_t38_start(0, 1) == CALL_T38_REFUSED);
	CHECK(call_t38_start(CFG_CHANNEL_MAX, 0) == CALL_T38_REFUSED);

	/* the busy answer, which the shell does not spend its one attempt on:
	 * the exchange this leg runs is the caller's own hold */
	CASE("t38: a leg with an exchange already running refuses it");
	CHECK(call_hold(0, 0));
	CHECK(call_t38_start(0, 0) == CALL_T38_BUSY);
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "18", "a=recvonly\r\n"));

	/*
	 * The carrier the image session replaces. line/line.c latches its first
	 * stage on the ATTEMPT, so a backend that refused the profile leaves
	 * the line gate open on a leg that never converted: without this
	 * refusal the next preamble would offer an image session for a fax
	 * this side is not carrying.
	 */
	CASE("t38: a leg whose passthrough the backend refused starts no upgrade");
	t38_call_up();
	med.fax_ret = HAL_UNSUPPORTED;
	CHECK(!call_fax_start(0, 0));
	med.fax_ret = HAL_OK;
	before = fk.sends;
	CHECK(call_t38_start(0, 0) == CALL_T38_REFUSED);
	CHECK_UINT(fk.sends, before);
	CHECK(!next_offer_is_image());
	CHECK(last_req(&ri));
	CHECK(has(ri.body, "m=audio "));
	CHECK(!has(ri.body, "m=image"));

	/* the passthrough of the answer tone comes first in the two cases
	 * below, so the option and the feature are the gates they read */
	CASE("t38: a channel with fax_t38_enable 0 refuses it");
	fx_reset();
	t38_enable(true);
	cfg.channel[0].fax_t38_enable = false;
	uac_up_codec("18 101", NULL);
	CHECK(call_fax_start(0, 0));
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "0 101", NULL));
	before = fk.sends;
	CHECK(call_t38_start(0, 0) == CALL_T38_REFUSED);
	CHECK_UINT(fk.sends, before);

	CASE("t38: and so does a backend without the T.38 feature");
	fx_reset();
	t38_enable(true);
	calls_hal_features_set(HAL_FEAT_G729);
	uac_up_codec("18 101", NULL);
	CHECK(call_fax_start(0, 0));
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "0 101", NULL));
	CHECK(call_t38_start(0, 0) == CALL_T38_REFUSED);

	CASE("t38: a session that already carries T.38 refuses a second upgrade");
	t38_session_up();
	before = fk.sends;
	CHECK(call_t38_start(0, 0) == CALL_T38_REFUSED);
	CHECK_UINT(fk.sends, before);

	/* 15. an initial INVITE is never an upgrade (the mid-call gate of sdp.c) */
	CASE("t38: an initial INVITE that names an image stream is answered audio");
	fx_reset();
	t38_enable(true);
	rx(invite_text("call-t38-in", NULL,
		       t38_body(T_PEER, 30000, "m=audio 20000 RTP/AVP 0\r\n", NULL)));
	CHECK(call_ring(0, 0));
	CHECK(call_answer(0, 0));
	CHECK(has(last_text(), "m=audio 10000 RTP/AVP 0"));
	CHECK(has(last_text(), "m=image 0 udptl t38\r\n"));
	CHECK_UINT(med.last.codec, HAL_CODEC_PCMU);

	/* 16. the five restore sites, one failure shape each */
	CASE("restore: a re-INVITE that could not be sent leaves the leg on audio");
	t38_passthrough_up();
	fk.send_fail = true;
	CHECK(call_t38_start(0, 0) == CALL_T38_REFUSED);
	fk.send_fail = false;
	CHECK(!next_offer_is_image());
	CHECK(last_req(&ri));
	CHECK(has(ri.body, "m=audio "));
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "0", "a=recvonly\r\n"));

	/* the next offer of the leg is the proof: one whose enable was not
	 * restored would offer an image section for a hold made after the
	 * refusal, while the committed session is the audio one */
	CASE("restore: a refused upgrade leaves the next offer on audio");
	t38_passthrough_up();
	CHECK(call_t38_start(0, 0) == CALL_T38_STARTED);
	reply(488, NULL, NULL, NULL);
	CHECK(!next_offer_is_image());
	CHECK(last_req(&ri));
	CHECK(has(ri.body, "m=audio "));
	CHECK(!has(ri.body, "m=image"));

	CASE("restore: a 2xx for a clearing leg hands the session back at once");
	t38_two_passthrough_up();
	CHECK(call_t38_start(0, 0) == CALL_T38_STARTED);
	before = fk.sends - 1;
	CHECK(call_hangup(0, 0));
	CHECK(call_t38_start(1, 0) == CALL_T38_REFUSED);
	reply_at(before, 200, NULL, NULL, t38_body(T_PEER, 30000, NULL, NULL));
	CHECK(call_t38_start(1, 0) == CALL_T38_STARTED);

	CASE("restore: a final for a clearing leg hands it back the same way");
	t38_two_passthrough_up();
	CHECK(call_t38_start(0, 0) == CALL_T38_STARTED);
	before = fk.sends - 1;
	CHECK(call_hangup(0, 0));
	CHECK(call_t38_start(1, 0) == CALL_T38_REFUSED);
	reply_at(before, 488, NULL, NULL, NULL);
	CHECK(call_t38_start(1, 0) == CALL_T38_STARTED);

	/*
	 * The glare restore, on the path that does not come back. A 491 arms a
	 * retry, and the retry re-applies the enable itself, so the restore is
	 * invisible while the retry succeeds. It is the leg whose retry never
	 * reaches the wire that reads it: without the restore that leg keeps an
	 * enable no exchange stands behind, holds the daemon-wide admission for
	 * the rest of the call, and offers an image stream on its next hold or
	 * refresh while the committed session is audio.
	 */
	CASE("restore: a 491 whose retry never leaves puts the leg back on audio");
	t38_two_passthrough_up();
	CHECK(call_t38_start(0, 0) == CALL_T38_STARTED);
	reply(491, NULL, NULL, NULL);
	fk.send_fail = true;
	fake_advance(4000);
	fk.send_fail = false;
	CHECK(!next_offer_is_image());
	CHECK(last_req(&ri));
	CHECK(has(ri.body, "m=audio "));
	CHECK(!has(ri.body, "m=image"));

	CASE("restore: and it handed the T.38 session back with it");
	CHECK(call_t38_start(1, 0) == CALL_T38_STARTED);

	/*
	 * The glare restore matters in the reverse direction too. A hold made
	 * during an image session must retry with the image offer it made the
	 * first time: a snapshot that was not taken, or not put back, would
	 * drop the far end to audio on a 491 while the fax runs.
	 */
	CASE("restore: a 491 on a hold made during T.38 retries with the image");
	t38_session_up();
	CHECK(call_hold(0, 0));
	CHECK(last_req(&ri));
	CHECK(has(ri.body, "m=image "));
	reply(491, NULL, NULL, NULL);
	fake_advance(5000);
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.method, "INVITE"));
	CHECK(has(ri.body, "m=image "));

	CASE("restore: a 491 on a refresh made during T.38 does the same");
	fx_reset();
	t38_enable(true);
	cfg.channel[0].session_max_expires = 1800;
	call_dial(0, 0, "1002", true);
	reply(100, NULL, NULL, NULL);
	reply(200, "peer-tag", "Session-Expires: 1800;refresher=uac\r\n",
	      sdp_body(T_PEER, 20000, "18 101", NULL));
	CHECK(call_fax_start(0, 0));
	reply(200, NULL, "Session-Expires: 1800;refresher=uac\r\n",
	      sdp_body(T_PEER, 20000, "0 101", NULL));
	CHECK(call_t38_start(0, 0) == CALL_T38_STARTED);
	reply(200, NULL, "Session-Expires: 1800;refresher=uac\r\n",
	      t38_body(T_PEER, 30000, NULL, NULL));
	fake_advance(900000);
	reply(491, NULL, NULL, NULL);
	fake_advance(4000);
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.method, "INVITE"));
	CHECK(has(ri.body, "m=image "));
	cfg.channel[0].session_max_expires = 0;

	/* the fixture is shared: every later case runs with T.38 off again */
	t38_enable(false);
}

/* ------------------------------------------------------------------- SRTP */

#define SUITE_80	"AES_CM_128_HMAC_SHA1_80"
#define SUITE_32	"AES_CM_128_HMAC_SHA1_32"

/*
 * The three gates a leg reads at its start (call.c leg_media_take): the base
 * config switch, the backend feature bit and the injected entropy op. A case
 * names all three before it dials, the way t38_enable() names the T.38 pair.
 * The switch lives in the shared cfg, so the last case of the block puts it
 * back.
 */
static void srtp_fx(bool sw, uint32_t features, bool op)
{
	fx_reset();
	cfg.srtp_enabled = sw;
	calls_hal_features_set(features);
	if (op)
		return;

	calls_done();
	net_port_reset();
	calls_init(&rec_ops_plain, &rec_cb);
	calls_reload(&cfg);
	calls_host_bind(proto_host_fake());
	calls_hal_ready_bind(fake_hal_ready);
	calls_hal_features_set(features);
	calls_hal_codecs_set(~0u);
}

/* the octets the scripted op hands out, from the next draw position on */
static void srtp_script(const uint8_t *bytes, size_t len)
{
	memcpy(med.rand, bytes, len);
	med.rand_len = len;
	med.rand_next = 0;
	med.rand_calls = 0;
}

/* the key the unscripted op gives suite idx + 1, drawn in one pass over both */
static void srtp_key_expect(uint8_t *out, unsigned int idx)
{
	unsigned int i;

	for (i = 0; i < SDP_SRTP_KEY_LEN; i++)
		out[i] = rand_fill_at(idx * SDP_SRTP_KEY_LEN + i);
}

static bool srtp_key_set_is(const uint8_t *key)
{
	unsigned int i;

	for (i = 0; i < SDP_SRTP_KEY_LEN; i++) {
		if (key[i])
			return true;
	}

	return false;
}

/*
 * RFC 4648 4, the encoding a crypto attribute carries. The suite computes it
 * itself: a case then names raw key octets and asserts the text on the wire,
 * and the sdp.c codec is never its own oracle. 30 octets are ten whole groups,
 * so no padding is written.
 */
static const char srtp_b64_alpha[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
				    "abcdefghijklmnopqrstuvwxyz0123456789+/";

static void srtp_key_b64(char *out, const uint8_t *key)
{
	const char *alpha = srtp_b64_alpha;
	unsigned int i;

	for (i = 0; i < SDP_SRTP_KEY_LEN; i += 3) {
		uint32_t v = ((uint32_t)key[i] << 16) | ((uint32_t)key[i + 1] << 8) |
			     key[i + 2];

		*out++ = alpha[(v >> 18) & 0x3f];
		*out++ = alpha[(v >> 12) & 0x3f];
		*out++ = alpha[(v >> 6) & 0x3f];
		*out++ = alpha[v & 0x3f];
	}

	*out = 0;
}

/*
 * The key that went on the wire, decoded straight out of the body the
 * implementation emitted, so a case checks the key material the leg sent and
 * not its own expectation.
 */
static bool srtp_line_key_get(const char *body, unsigned int tag, uint8_t *out)
{
	char want[24];
	const char *p, *t;
	unsigned int i;

	snprintf(want, sizeof(want), "a=crypto:%u ", tag);
	p = strstr(body, want);
	if (!p || !(t = strstr(p, "inline:")))
		return false;

	t += 7;
	for (i = 0; i < SDP_SRTP_KEY_LEN; i += 3) {
		uint32_t v = 0;
		unsigned int k;

		for (k = 0; k < 4; k++, t++) {
			const char *at = t[0] ? strchr(srtp_b64_alpha, t[0]) : NULL;

			if (!at)
				return false;
			v = (v << 6) | (uint32_t)(at - srtp_b64_alpha);
		}

		out[i] = (uint8_t)(v >> 16);
		out[i + 1] = (uint8_t)(v >> 8);
		out[i + 2] = (uint8_t)v;
	}

	return true;
}

static const char *srtp_crypto_line(unsigned int tag, const char *suite, const uint8_t *key)
{
	char *buf = text_slot();
	char b64[64];

	srtp_key_b64(b64, key);
	snprintf(buf, TEXT_MAX, "a=crypto:%u %s inline:%s\r\n", tag, suite, b64);

	return buf;
}

/* how many crypto attributes a body carries */
static unsigned int srtp_crypto_count(const char *body)
{
	const char *p = body;
	unsigned int n = 0;

	while ((p = strstr(p, "a=crypto:")) != NULL) {
		n++;
		p += 9;
	}

	return n;
}

/* an RTP/SAVP offer or answer, with the crypto block the caller built */
static const char *savp_body(const char *addr, unsigned int port, const char *pts,
			     const char *crypto, const char *extra)
{
	char *buf = text_slot();

	snprintf(buf, TEXT_MAX,
		 "v=0\r\n"
		 "o=- 42 42 IN IP4 %s\r\n"
		 "s=-\r\n"
		 "c=IN IP4 %s\r\n"
		 "t=0 0\r\n"
		 "m=audio %u RTP/SAVP %s\r\n"
		 "%s"
		 "a=rtpmap:0 PCMU/8000\r\n"
		 "a=rtpmap:8 PCMA/8000\r\n"
		 "a=rtpmap:101 telephone-event/8000\r\n"
		 "%s",
		 addr, addr, port, pts, crypto ? crypto : "", extra ? extra : "");

	return buf;
}

/* an inbound leg answered on an SAVP offer, which is where the offerless
 * re-INVITE cases of the rekey stop start */
static void srtp_uas_up(const char *call_id, const uint8_t *peer_key)
{
	rx(invite_text(call_id, NULL,
		       savp_body(T_PEER, 20000, "0 101",
				 srtp_crypto_line(1, SUITE_80, peer_key), NULL)));
	call_ring(0, 0);
	call_answer(0, 0);
	rx(uas_in_dialog("ACK", 1, NULL, NULL));
}

/* an outbound leg answered with an SAVP answer that mirrors one of our tags */
static void srtp_uac_up(unsigned int tag, const char *suite, const uint8_t *peer_key)
{
	call_dial(0, 0, "1002", true);
	reply(100, NULL, NULL, NULL);
	reply(180, "peer-tag", NULL, NULL);
	reply(200, "peer-tag", NULL,
	      savp_body(T_PEER, 20000, "0 101", srtp_crypto_line(tag, suite, peer_key), NULL));
}

/*
 * SDES SRTP on a call leg: sdp.c owns the negotiation itself, and this group
 * covers everything that the leg owns. The leg reads three gates at its
 * start. It draws two master keys through the injected entropy op. The leg
 * puts the key material into the stream description, and takes a
 * stop-configure-start for a rekey. It refuses the T.38 upspeed, and its
 * status reports the suite.
 */
static void test_srtp(void)
{
	uint8_t want0[SDP_SRTP_KEY_LEN], want1[SDP_SRTP_KEY_LEN];
	uint8_t peer[SDP_SRTP_KEY_LEN], peer2[SDP_SRTP_KEY_LEN];
	uint8_t got0[SDP_SRTP_KEY_LEN], got1[SDP_SRTP_KEY_LEN];
	uint8_t script[2 * SDP_SRTP_KEY_LEN];
	unsigned int i, stops, configs, starts, before;
	struct cfg_channel ch;
	struct sdp_result res;
	struct hal_media m;
	struct slot_view sv;
	struct req_info ri;
	char crypto2[256];

	for (i = 0; i < SDP_SRTP_KEY_LEN; i++) {
		peer[i] = (uint8_t)(0x40 + i);
		peer2[i] = (uint8_t)(0x80 + i);
	}

	/* 1. the three gates of the leg start */
	srtp_fx(true, HAL_FEAT_SRTP, true);
	srtp_key_expect(want0, 0);
	srtp_key_expect(want1, 1);

	CASE("srtp: an enabled leg offers RTP/SAVP");
	CHECK(call_dial(0, 0, "1002", true));
	CHECK(last_req(&ri));
	CHECK(has(ri.body, "m=audio 10000 RTP/SAVP 0 8 18 101"));

	CASE("srtp: with one crypto line per suite, keyed from the entropy op");
	CHECK_UINT(srtp_crypto_count(ri.body), 2);
	CHECK(has(ri.body, srtp_crypto_line(1, SUITE_80, want0)));
	CHECK(has(ri.body, srtp_crypto_line(2, SUITE_32, want1)));

	CASE("srtp: drawn with one op call per key");
	CHECK_UINT(med.rand_calls, 2);

	CASE("srtp: a backend without the feature bit offers plain RTP");
	srtp_fx(true, 0, true);
	CHECK(call_dial(0, 0, "1002", true));
	CHECK(last_req(&ri));
	CHECK(has(ri.body, "m=audio 10000 RTP/AVP 0 8 18 101"));
	CHECK_UINT(srtp_crypto_count(ri.body), 0);
	CHECK_UINT(med.rand_calls, 0);

	CASE("srtp: and so does a leg whose config switch is off");
	srtp_fx(false, HAL_FEAT_SRTP, true);
	CHECK(call_dial(0, 0, "1002", true));
	CHECK(last_req(&ri));
	CHECK(has(ri.body, "m=audio 10000 RTP/AVP 0 8 18 101"));
	CHECK_UINT(srtp_crypto_count(ri.body), 0);
	CHECK_UINT(med.rand_calls, 0);

	CASE("srtp: a caller that bound no entropy op offers plain RTP, once traced");
	srtp_fx(true, HAL_FEAT_SRTP, false);
	CHECK(call_dial(0, 0, "1002", true));
	CHECK(last_req(&ri));
	CHECK(has(ri.body, "m=audio 10000 RTP/AVP 0 8 18 101"));
	CHECK_UINT(srtp_crypto_count(ri.body), 0);
	CHECK_UINT(tr.srtp_plain_lines, 1);

	/* 2. the draw keeps every octet value */
	srtp_fx(true, HAL_FEAT_SRTP, true);
	for (i = 0; i < sizeof(script); i++)
		script[i] = (uint8_t)(i + 1);
	script[0] = 0;
	script[28] = 0;
	script[SDP_SRTP_KEY_LEN] = 0;
	script[2 * SDP_SRTP_KEY_LEN - 1] = 0;
	srtp_script(script, sizeof(script));

	CASE("srtp: both keys go on the wire as drawn, 0x00 at any byte included");
	CHECK(call_dial(0, 0, "1002", true));
	CHECK(last_req(&ri));
	CHECK(srtp_line_key_get(ri.body, 1, got0));
	CHECK(srtp_line_key_get(ri.body, 2, got1));
	CHECK(!memcmp(got0, script, SDP_SRTP_KEY_LEN));
	CHECK(!memcmp(got1, script + SDP_SRTP_KEY_LEN, SDP_SRTP_KEY_LEN));
	CHECK_UINT(med.rand_calls, 2);

	/* 3. an entropy failure fails the leg, like a missing RTP port */
	CASE("srtp: an outbound leg whose entropy op refuses fails congestion");
	srtp_fx(true, HAL_FEAT_SRTP, true);
	med.rand_fail = true;
	CHECK(!call_dial(0, 0, "1002", true));
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.cls, CALL_FAIL_CONGESTION);
	CHECK_UINT(fk.sends, 0);
	CHECK_UINT(med.config_calls, 0);

	CASE("srtp: and an inbound INVITE is answered 486");
	srtp_fx(true, HAL_FEAT_SRTP, true);
	med.rand_fail = true;
	rx(invite_text("call-srtp-dry", NULL, sdp_body(T_PEER, 20000, "0 101", NULL)));
	CHECK(has(last_text(), "SIP/2.0 486"));
	CHECK_UINT(ev.incoming, 0);
	CHECK_UINT(med.config_calls, 0);

	/* 4. an inbound SAVP offer, answered and programmed. The offer carries
	 * two crypto lines, so an answer that mirrors every line fails the
	 * count. */
	srtp_fx(true, HAL_FEAT_SRTP, true);
	srtp_key_expect(want0, 0);
	srtp_key_expect(want1, 1);
	snprintf(crypto2, sizeof(crypto2), "%s%s",
		 srtp_crypto_line(1, SUITE_80, peer),
		 srtp_crypto_line(2, SUITE_32, peer2));
	rx(invite_text("call-srtp-in", NULL,
		       savp_body(T_PEER, 20000, "0 101", crypto2, NULL)));
	CHECK(call_ring(0, 0));
	CHECK(call_answer(0, 0));

	CASE("srtp: an inbound SAVP offer is answered with ONE mirrored crypto line");
	CHECK(has(last_text(), "m=audio 10000 RTP/SAVP 0"));
	CHECK_UINT(srtp_crypto_count(last_text()), 1);
	CHECK(has(last_text(), srtp_crypto_line(1, SUITE_80, want0)));

	CASE("srtp: and the stream carries that suite with both keys");
	CHECK_UINT(med.last.srtp.suite, HAL_SRTP_AES_CM_128_SHA1_80);
	CHECK(!memcmp(med.last.srtp.key_tx, want0, SDP_SRTP_KEY_LEN));
	CHECK(!memcmp(med.last.srtp.key_rx, peer, SDP_SRTP_KEY_LEN));

	/* 5. a rekey on a running stream */
	rx(uas_in_dialog("ACK", 1, NULL, NULL));
	med.nseq = 0;
	stops = med.stop_calls;
	rx(uas_in_dialog("INVITE", 2, NULL,
			 savp_body(T_PEER, 20000, "0 101",
				   srtp_crypto_line(1, SUITE_80, peer2), NULL)));

	CASE("srtp: a re-INVITE with a NEW key stops, configures and starts");
	CHECK(med_seq_index(MED_STOP) == 0);
	CHECK(med_seq_index(MED_CONFIG) == 1);
	CHECK(med_seq_index(MED_START) == 2);
	CHECK_UINT(med.stop_calls, stops + 1);

	CASE("srtp: and the stream then opens with the peer's new key");
	CHECK(!memcmp(med.last.srtp.key_rx, peer2, SDP_SRTP_KEY_LEN));
	CHECK(!memcmp(med.last.srtp.key_tx, want0, SDP_SRTP_KEY_LEN));

	rx(uas_in_dialog("ACK", 2, NULL, NULL));
	med.nseq = 0;
	stops = med.stop_calls;
	configs = med.config_calls;
	rx(uas_in_dialog("INVITE", 3, NULL,
			 savp_body(T_PEER, 20000, "0 101",
				   srtp_crypto_line(1, SUITE_80, peer2), NULL)));

	CASE("srtp: an identical re-offer takes neither a stop nor a configure");
	CHECK_UINT(med.stop_calls, stops);
	CHECK_UINT(med.config_calls, configs);
	CHECK(med_seq_index(MED_STOP) < 0);
	CHECK(med_seq_index(MED_CONFIG) < 0);

	rx(uas_in_dialog("ACK", 3, NULL, NULL));
	med.nseq = 0;
	stops = med.stop_calls;
	rx(uas_in_dialog("INVITE", 4, NULL,
			 savp_body(T_PEER, 20000, "0 101",
				   srtp_crypto_line(2, SUITE_32, peer2), NULL)));

	CASE("srtp: a re-offer that moves the SUITE takes the same path");
	CHECK(med_seq_index(MED_STOP) == 0);
	CHECK(med_seq_index(MED_CONFIG) == 1);
	CHECK(med_seq_index(MED_START) == 2);
	CHECK_UINT(med.stop_calls, stops + 1);

	CASE("srtp: and the stream carries the other suite under our other key");
	CHECK_UINT(med.last.srtp.suite, HAL_SRTP_AES_CM_128_SHA1_32);
	CHECK(!memcmp(med.last.srtp.key_tx, want1, SDP_SRTP_KEY_LEN));

	/*
	 * 5a. Accept-plain narrows to the initial offer. A plain re-offer on a
	 * keyed leg would otherwise commit the downgrade: the stream would be
	 * programmed again with HAL_SRTP_NONE, nothing would report it, and
	 * the call would run in clear. sdp.c refuses the answer, rx_reinvite
	 * turns that into the 488 and the rollback, and the stream is never
	 * touched: the ops below are the whole record of it.
	 */
	srtp_fx(true, HAL_FEAT_SRTP, true);
	srtp_key_expect(want0, 0);
	srtp_uas_up("call-srtp-plain", peer);
	med.nseq = 0;
	stops = med.stop_calls;
	configs = med.config_calls;
	starts = med.start_calls;
	rx(uas_in_dialog("INVITE", 2, NULL, sdp_body(T_PEER, 20000, "0 101", NULL)));

	CASE("srtp: a plain re-INVITE on a keyed leg answers 488 and keeps the stream");
	CHECK(has(last_text(), "SIP/2.0 488"));
	CHECK_UINT(med.stop_calls, stops);
	CHECK_UINT(med.config_calls, configs);
	CHECK_UINT(med.start_calls, starts);
	CHECK(med_seq_index(MED_STOP) < 0);
	CHECK(med_seq_index(MED_CONFIG) < 0);
	CHECK(med_seq_index(MED_START) < 0);

	CASE("srtp: and the committed suite and both keys are still the ones it runs");
	CHECK_UINT(med.last.srtp.suite, HAL_SRTP_AES_CM_128_SHA1_80);
	CHECK(!memcmp(med.last.srtp.key_tx, want0, SDP_SRTP_KEY_LEN));
	CHECK(!memcmp(med.last.srtp.key_rx, peer, SDP_SRTP_KEY_LEN));
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.state, "active"));
	CHECK(!strcmp(sv.srtp, SUITE_80));

	CASE("srtp: and the next SAVP re-INVITE still rekeys that leg");
	med.nseq = 0;
	stops = med.stop_calls;
	rx(uas_in_dialog("INVITE", 3, NULL,
			 savp_body(T_PEER, 20000, "0 101",
				   srtp_crypto_line(1, SUITE_80, peer2), NULL)));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(med_seq_index(MED_STOP) == 0);
	CHECK(med_seq_index(MED_CONFIG) == 1);
	CHECK(med_seq_index(MED_START) == 2);
	CHECK_UINT(med.stop_calls, stops + 1);
	CHECK(!memcmp(med.last.srtp.key_rx, peer2, SDP_SRTP_KEY_LEN));

	/* the exchange only commits at the ACK, so the recovery is not proven
	 * until the leg is back in its steady state under the new key */
	CASE("srtp: and its ACK commits the new key on an active leg");
	rx(uas_in_dialog("ACK", 3, NULL, NULL));
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.state, "active"));
	CHECK(!strcmp(sv.srtp, SUITE_80));
	CHECK_UINT(med.last.srtp.suite, HAL_SRTP_AES_CM_128_SHA1_80);
	CHECK(!memcmp(med.last.srtp.key_tx, want0, SDP_SRTP_KEY_LEN));
	CHECK(!memcmp(med.last.srtp.key_rx, peer2, SDP_SRTP_KEY_LEN));

	/*
	 * 5b. The rekey stop on the offerless-ACK path (uas_ack_take). A later
	 * exchange records a refused configuration and keeps the call, because
	 * the old stream still runs. A rekey stops the stream first, so the
	 * three arms below are: the stream went down and the call ends, the
	 * stop itself refused and the old stream carries the call on, and the
	 * no-rekey arm that only records.
	 */
	srtp_fx(true, HAL_FEAT_SRTP, true);
	srtp_uas_up("call-srtp-oa", peer);
	rx(uas_in_dialog("INVITE", 2, NULL, NULL));
	stops = med.stop_calls;
	starts = med.start_calls;
	med.config_ret = HAL_FATAL;
	rx(uas_in_dialog("ACK", 2, NULL,
			 savp_body(T_PEER, 20000, "0 101",
				   srtp_crypto_line(1, SUITE_80, peer2), NULL)));

	CASE("srtp: a configure refused behind a rekey stop ends the call");
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.cls, CALL_FAIL_CONGESTION);
	CHECK_UINT(med.stop_calls, stops + 1);
	CHECK_UINT(med.start_calls, starts);
	CHECK(!slot_view_get(0, 0, &sv) || strcmp(sv.state, "active"));

	srtp_fx(true, HAL_FEAT_SRTP, true);
	srtp_uas_up("call-srtp-ob", peer);
	rx(uas_in_dialog("INVITE", 2, NULL, NULL));
	starts = med.start_calls;
	med.stop_ret = HAL_FATAL;
	rx(uas_in_dialog("ACK", 2, NULL,
			 savp_body(T_PEER, 20000, "0 101",
				   srtp_crypto_line(1, SUITE_80, peer2), NULL)));

	CASE("srtp: while a REFUSED stop leaves the old stream carrying the call");
	CHECK_UINT(ev.failed, 0);
	CHECK_UINT(med.start_calls, starts);
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.state, "active"));
	CHECK(!memcmp(med.last.srtp.key_rx, peer, SDP_SRTP_KEY_LEN));
	med.stop_ret = HAL_OK;

	srtp_fx(false, HAL_FEAT_SRTP, true);
	uas_up("call-plain-oc");
	rx(uas_in_dialog("INVITE", 2, NULL, NULL));
	stops = med.stop_calls;
	med.config_ret = HAL_FATAL;
	rx(uas_in_dialog("ACK", 2, NULL, sdp_body(T_PEER, 22000, "8", NULL)));

	CASE("srtp: and a plain leg with no rekey still only records the refusal");
	CHECK_UINT(ev.failed, 0);
	CHECK_UINT(med.stop_calls, stops);
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.state, "active"));
	med.config_ret = HAL_OK;

	/* 6. the T.38 upspeed is gated off on an SRTP leg */
	srtp_fx(true, HAL_FEAT_SRTP, true);
	t38_enable(true);
	calls_hal_features_set(HAL_FEAT_SRTP | HAL_FEAT_T38);
	srtp_key_expect(want0, 0);
	srtp_uac_up(1, SUITE_80, peer);

	CASE("srtp: an inbound T.38 upgrade is refused at port 0 on an SRTP leg");
	rx(uac_in_dialog("INVITE", "peer-tag", 2, NULL,
			 savp_body(T_PEER, 20000, "0 101",
				   srtp_crypto_line(1, SUITE_80, peer),
				   "m=image 30002 udptl t38\r\n")));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(has(last_text(), "m=image 0 udptl t38"));

	CASE("srtp: and the leg keeps running its SRTP audio");
	rx(uac_in_dialog("ACK", "peer-tag", 2, NULL, NULL));
	CHECK_UINT(med.last.codec, HAL_CODEC_PCMU);
	CHECK_UINT(med.last.srtp.suite, HAL_SRTP_AES_CM_128_SHA1_80);
	CHECK(!memcmp(med.last.srtp.key_tx, want0, SDP_SRTP_KEY_LEN));

	CASE("srtp: the answer tone still starts the G.711 passthrough");
	CHECK(call_fax_start(0, 0));
	CHECK_UINT(med.fax_calls, 1);
	reply(200, "peer-tag", NULL,
	      savp_body(T_PEER, 20000, "0 101", srtp_crypto_line(1, SUITE_80, peer), NULL));
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.fax, "passthrough"));

	CASE("srtp: but the T.38 upgrade puts no re-INVITE on the wire");
	before = fk.sends;
	CHECK(call_t38_start(0, 0) == CALL_T38_REFUSED);
	CHECK_UINT(fk.sends, before);
	CHECK_UINT(tr.srtp_t38_lines, 1);

	CASE("srtp: and the passthrough it was running stays");
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.fax, "passthrough"));
	CHECK_UINT(med.last.codec, HAL_CODEC_PCMU);

	CASE("srtp: a PLAIN leg with the same two gates still upgrades");
	srtp_fx(false, HAL_FEAT_SRTP, true);
	t38_enable(true);
	calls_hal_features_set(HAL_FEAT_SRTP | HAL_FEAT_T38);
	uac_up_codec("18 101", NULL);
	CHECK(call_fax_start(0, 0));
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "0 101", NULL));
	CHECK(call_t38_start(0, 0) == CALL_T38_STARTED);
	CHECK_UINT(tr.srtp_t38_lines, 0);
	t38_enable(false);

	/*
	 * 7. the fill, driven directly. The T.38 arm forces the suite to NONE
	 * whatever the result carries, and no gate above ever lets a T.38
	 * result reach an SRTP leg, so this is the only oracle that arm has.
	 */
	memset(&ch, 0, sizeof(ch));
	ch.ptime_ms = 20;
	ch.jitter_target = 50;
	ch.jitter_max = 100;
	ch.dtmf_mode = CFG_DTMF_RFC2833;

	memset(&res, 0, sizeof(res));
	res.codec.id = SDP_CODEC_PCMU;
	res.dtmf_pt = -1;
	res.cn_pt = -1;
	res.ptime_ms = 20;
	res.remote_ipver = 4;
	res.remote_port = 20000;
	snprintf(res.remote_addr, sizeof(res.remote_addr), "%s", T_PEER);
	res.srtp_suite = SDP_SRTP_AES_CM_128_SHA1_80;
	memset(res.srtp_key_tx, 0x5a, SDP_SRTP_KEY_LEN);
	memset(res.srtp_key_rx, 0x5b, SDP_SRTP_KEY_LEN);
	res.valid = true;

	CASE("srtp: the fill copies the suite and both keys of an audio result");
	media_fill(&ch, false, 0, false, &res, T_HOST, 10000, "",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.srtp.suite, HAL_SRTP_AES_CM_128_SHA1_80);
	CHECK(!memcmp(m.srtp.key_tx, res.srtp_key_tx, SDP_SRTP_KEY_LEN));
	CHECK(!memcmp(m.srtp.key_rx, res.srtp_key_rx, SDP_SRTP_KEY_LEN));

	CASE("srtp: and forces plain on a T.38 result that carries a suite");
	res.t38 = true;
	media_fill(&ch, false, 0, false, &res, T_HOST, 10000, "",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.codec, HAL_CODEC_T38);
	CHECK_UINT(m.srtp.suite, HAL_SRTP_NONE);
	CHECK(!srtp_key_set_is(m.srtp.key_tx));
	CHECK(!srtp_key_set_is(m.srtp.key_rx));

	/* 8. the status dump */
	CASE("srtp: the status of an SRTP call names the suite");
	srtp_fx(true, HAL_FEAT_SRTP, true);
	srtp_uac_up(2, SUITE_32, peer);
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.srtp, SUITE_32));

	CASE("srtp: while a plain call carries no such key");
	srtp_fx(false, HAL_FEAT_SRTP, true);
	uac_up(NULL);
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!sv.srtp[0]);
	CHECK(!strcmp(sv.codec, "PCMU"));

	/*
	 * 9. Our re-offers follow the committed transport. An accept-plain leg
	 * is the case that matters: the line carries the enable, the peer
	 * offered plain and we answered plain, so the call runs unencrypted.
	 * Re-offering RTP/SAVP after that asks a peer with no SRTP for a
	 * transport it already declined; it answers 488, our hold reports a
	 * failure and the first session refresh clears the call.
	 */
	srtp_fx(true, HAL_FEAT_SRTP, true);
	rx(invite_text("call-ap-plain", "Session-Expires: 1800;refresher=uas\r\n",
		       sdp_body(T_PEER, 20000, "0 101", NULL)));
	CHECK(call_ring(0, 0));
	CHECK(call_answer(0, 0));

	CASE("srtp: an enabled line answers a plain first offer plain");
	CHECK(has(last_text(), "m=audio 10000 RTP/AVP 0"));
	CHECK_UINT(srtp_crypto_count(last_text()), 0);
	rx(uas_in_dialog("ACK", 1, NULL, NULL));
	CHECK_UINT(med.last.srtp.suite, HAL_SRTP_NONE);

	CASE("srtp: and its hold re-INVITE offers RTP/AVP with no crypto attribute");
	CHECK(call_hold(0, 0));
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.method, "INVITE"));
	CHECK(has(ri.body, "m=audio 10000 RTP/AVP 0"));
	CHECK_UINT(srtp_crypto_count(ri.body), 0);
	CHECK(has(ri.body, "a=sendonly"));
	before = fk.sends - 1;

	CASE("srtp: and the peer's plain answer completes the hold");
	reply_at(before, 200, NULL, "Session-Expires: 1800;refresher=uac\r\n",
		 sdp_body(T_PEER, 20000, "0 101", "a=recvonly\r\n"));
	CHECK_UINT(ev.reinvite_done, 1);
	CHECK(ev.reinvite_ok);
	CHECK_UINT(med.last.direction, HAL_MEDIA_SENDONLY);
	CHECK_UINT(med.last.srtp.suite, HAL_SRTP_NONE);

	CASE("srtp: and the session refresh behind it keeps the call up");
	fake_advance(900000);
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.method, "INVITE"));
	CHECK(has(ri.body, "m=audio 10000 RTP/AVP 0"));
	CHECK_UINT(srtp_crypto_count(ri.body), 0);
	before = fk.sends - 1;
	reply_at(before, 200, NULL, "Session-Expires: 1800;refresher=uac\r\n",
		 sdp_body(T_PEER, 20000, "0 101", "a=recvonly\r\n"));
	CHECK_UINT(ev.failed, 0);
	CHECK(find_send("BYE") < 0);
	CHECK(slot_view_get(0, 0, &sv));
	CHECK(!strcmp(sv.state, "active"));

	CASE("srtp: a KEYED leg still holds with an SAVP re-offer carrying both keys");
	srtp_fx(true, HAL_FEAT_SRTP, true);
	srtp_key_expect(want0, 0);
	srtp_key_expect(want1, 1);
	srtp_uas_up("call-srtp-hold", peer);
	CHECK(call_hold(0, 0));
	CHECK(last_req(&ri));
	CHECK(has(ri.body, "m=audio 10000 RTP/SAVP 0"));
	CHECK_UINT(srtp_crypto_count(ri.body), 2);
	CHECK(has(ri.body, srtp_crypto_line(1, SUITE_80, want0)));
	CHECK(has(ri.body, srtp_crypto_line(2, SUITE_32, want1)));
	CHECK(has(ri.body, "a=sendonly"));
	before = fk.sends - 1;

	CASE("srtp: and its mirrored SAVP answer completes that hold");
	reply_at(before, 200, NULL, NULL,
		 savp_body(T_PEER, 20000, "0 101", srtp_crypto_line(1, SUITE_80, peer),
			   "a=recvonly\r\n"));
	CHECK_UINT(ev.reinvite_done, 1);
	CHECK(ev.reinvite_ok);
	CHECK_UINT(med.last.direction, HAL_MEDIA_SENDONLY);
	CHECK_UINT(med.last.srtp.suite, HAL_SRTP_AES_CM_128_SHA1_80);
	CHECK(!memcmp(med.last.srtp.key_tx, want0, SDP_SRTP_KEY_LEN));
	CHECK(!memcmp(med.last.srtp.key_rx, peer, SDP_SRTP_KEY_LEN));

	/* the fixture is shared: every later case runs with the switch off */
	cfg.srtp_enabled = false;
}

/*
 * The key wipe of a released leg. A slot is static storage that the next
 * allocation memsets. Without the wipe, a dead leg would keep its master keys
 * readable for as long as the line stays idle. Nothing else that this suite
 * can call reaches the slot, so call_slot_scan() exists for exactly these
 * cases (call.h). Each case ends a leg, then sweeps the whole slot for both of
 * our master keys and the key of the peer. It also sweeps for the base64 text
 * of the a=crypto line that carried them.
 */
static void test_srtp_wipe(void)
{
	uint8_t mine0[SDP_SRTP_KEY_LEN], mine1[SDP_SRTP_KEY_LEN];
	uint8_t peer[SDP_SRTP_KEY_LEN];
	uint8_t script[2 * SDP_SRTP_KEY_LEN];
	char b64[64];
	unsigned int i;

	for (i = 0; i < SDP_SRTP_KEY_LEN; i++) {
		script[i] = (uint8_t)(0x11 + i);
		script[SDP_SRTP_KEY_LEN + i] = (uint8_t)(0x71 + i);
		peer[i] = (uint8_t)(0xc1 + i);
	}
	memcpy(mine0, script, SDP_SRTP_KEY_LEN);
	memcpy(mine1, script + SDP_SRTP_KEY_LEN, SDP_SRTP_KEY_LEN);

	/* 1. an inbound leg the peer ended with a BYE */
	srtp_fx(true, HAL_FEAT_SRTP, true);
	srtp_script(script, sizeof(script));
	srtp_uas_up("call-wipe-uas", peer);

	CASE("wipe: the live inbound slot holds the key material it negotiated");
	CHECK(call_slot_scan(0, 0, mine0, SDP_SRTP_KEY_LEN));
	CHECK(call_slot_scan(0, 0, peer, SDP_SRTP_KEY_LEN));

	rx(uas_in_dialog("BYE", 2, NULL, NULL));

	CASE("wipe: a leg the peer BYEd keeps neither of our master keys");
	CHECK(!call_slot_scan(0, 0, mine0, SDP_SRTP_KEY_LEN));
	CHECK(!call_slot_scan(0, 0, mine1, SDP_SRTP_KEY_LEN));

	CASE("wipe: nor the peer's");
	CHECK(!call_slot_scan(0, 0, peer, SDP_SRTP_KEY_LEN));

	CASE("wipe: nor the base64 text any of the three crossed as");
	srtp_key_b64(b64, mine0);
	CHECK(!call_slot_scan(0, 0, b64, strlen(b64)));
	srtp_key_b64(b64, mine1);
	CHECK(!call_slot_scan(0, 0, b64, strlen(b64)));
	srtp_key_b64(b64, peer);
	CHECK(!call_slot_scan(0, 0, b64, strlen(b64)));

	/* 2. an outbound leg this side ended with a hangup */
	srtp_fx(true, HAL_FEAT_SRTP, true);
	srtp_script(script, sizeof(script));
	srtp_uac_up(2, SUITE_32, peer);

	CASE("wipe: the live outbound slot holds its key material too");
	CHECK(call_slot_scan(0, 0, mine1, SDP_SRTP_KEY_LEN));
	CHECK(call_slot_scan(0, 0, peer, SDP_SRTP_KEY_LEN));

	CHECK(call_hangup(0, 0));
	reply(200, "peer-tag", NULL, NULL);
	fake_advance(6000);

	CASE("wipe: a leg our own hangup ended keeps neither of our master keys");
	CHECK(!call_slot_scan(0, 0, mine0, SDP_SRTP_KEY_LEN));
	CHECK(!call_slot_scan(0, 0, mine1, SDP_SRTP_KEY_LEN));

	CASE("wipe: nor the peer's, nor the base64 of either of ours");
	CHECK(!call_slot_scan(0, 0, peer, SDP_SRTP_KEY_LEN));
	srtp_key_b64(b64, mine0);
	CHECK(!call_slot_scan(0, 0, b64, strlen(b64)));
	srtp_key_b64(b64, mine1);
	CHECK(!call_slot_scan(0, 0, b64, strlen(b64)));

	/* 3. a draw that failed on the second key: the leg never reached the
	 * wire, and the half-drawn material still has to go */
	srtp_fx(true, HAL_FEAT_SRTP, true);
	srtp_script(script, sizeof(script));
	med.rand_max = 1;

	CASE("wipe: an outbound leg whose second draw failed sends nothing");
	CHECK(!call_dial(0, 0, "1002", true));
	CHECK_UINT(fk.sends, 0);
	CHECK_UINT(med.rand_calls, 2);

	CASE("wipe: and its half-drawn key material is gone from the slot");
	CHECK(!call_slot_scan(0, 0, mine0, SDP_SRTP_KEY_LEN));
	CHECK(!call_slot_scan(0, 0, mine0, 8));

	med.rand_max = 0;
	cfg.srtp_enabled = false;
}

/*
 * The device that the media sockets scope to. It comes from the account
 * view, which resolves the logical interface as the SIP sockets do. The two
 * forks can therefore never bind to different netdevs.
 */
static void test_media_device(void)
{
	CASE("media: the bind device comes from the view");
	fx_reset();
	fk.bind_dev = "lan4";
	uac_up(NULL);
	CHECK_UINT(med.config_calls, 1);
	CHECK(bind_dev_is("lan4"));

	CASE("media: an inbound leg takes the view device");
	fx_reset();
	fk.bind_dev = "lan4";
	uas_up("call-dev1");
	CHECK_UINT(med.config_calls, 1);
	CHECK(bind_dev_is("lan4"));

	CASE("media: an unresolved device leaves the stream unscoped");
	fx_reset();
	fk.bind_dev = NULL;
	uac_up(NULL);
	CHECK_UINT(med.config_calls, 1);
	CHECK_UINT(med.last.bind_dev[0], 0);

	/*
	 * The leg copied the device at its creation and keeps it for its whole
	 * life: hal.h routes every change of the local binding through a new
	 * socket pair, and an address move ends the call through
	 * account_ip_changed() instead.
	 */
	CASE("media: a re-INVITE keeps the device the leg took");
	fx_reset();
	fk.bind_dev = "lan4";
	uac_up(NULL);
	fk.bind_dev = "lan5";
	CHECK(call_hold(0, 0));
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "0", "a=recvonly\r\n"));
	CHECK_UINT(med.last.direction, HAL_MEDIA_SENDONLY);
	CHECK(call_resume(0, 0));
	reply(200, NULL, NULL, sdp_body(T_PEER, 20000, "0 101", NULL));
	CHECK_UINT(med.last.direction, HAL_MEDIA_SENDRECV);
	CHECK(bind_dev_is("lan4"));
}

/*
 * The media address on the wire. The fill refuses a remote description that
 * the address layer cannot honor, so media_program() fails. The leg then
 * takes the existing refused media path, leg_abandon() with the congestion
 * class. A HAL that refuses the stream config takes the same path. The
 * RFC 2543 c=0.0.0.0 hold form is a literal and stays a direction change, so no
 * refusal must reach it.
 */
static void test_media_addr(void)
{
	CASE("addr: an answer whose c= line names a host programs no stream");
	fx_reset();
	CHECK(dial(0, 0, "1002"));
	reply(100, NULL, NULL, NULL);
	reply(200, "peer-tag", NULL, sdp_body("sip.example.com", 20000, "0 101", NULL));
	CHECK_UINT(med.config_calls, 0);
	CHECK_UINT(med.start_calls, 0);

	CASE("addr: and the leg ends through the refused media class");
	CHECK_UINT(ev.answered, 0);
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.cls, CALL_FAIL_CONGESTION);
	CHECK(find_send("BYE") > 0);

	CASE("addr: the refused fill leaves no stream to stop or to snapshot");
	CHECK_UINT(med.stop_calls, 0);
	CHECK_UINT(med.snapshot_calls, 0);

	CASE("addr: the BYE releases the leg, with no media in the record");
	reply(200, NULL, NULL, NULL);
	fake_advance(6000);
	CHECK_UINT(ev.ended, 1);
	CHECK(ev.end.failed);
	CHECK(!ev.end.answered);
	CHECK(!ev.end.media_ran);
	CHECK_UINT(ev.end.cls, CALL_FAIL_CONGESTION);
	CHECK(slot_is(0));

	CASE("addr: a remote RTP port of 65535 programs no stream either");
	fx_reset();
	CHECK(dial(0, 0, "1002"));
	reply(100, NULL, NULL, NULL);
	reply(200, "peer-tag", NULL, sdp_body(T_PEER, 65535, "0 101", NULL));
	CHECK_UINT(med.config_calls, 0);
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.cls, CALL_FAIL_CONGESTION);
	CHECK(find_send("BYE") > 0);

	CASE("addr: 65534 is answered and carries 65535 as its RTCP port");
	fx_reset();
	CHECK(dial(0, 0, "1002"));
	reply(100, NULL, NULL, NULL);
	reply(200, "peer-tag", NULL, sdp_body(T_PEER, 65534, "0 101", NULL));
	CHECK_UINT(ev.answered, 1);
	CHECK_UINT(ev.failed, 0);
	CHECK_UINT(med.config_calls, 1);
	CHECK_UINT(med.start_calls, 1);
	CHECK_UINT(med.last.remote.port, 65534);
	CHECK_UINT(med.last.remote.rtcp_port, 65535);

	CASE("addr: a c=0.0.0.0 hold offer is answered recvonly, not refused");
	fx_reset();
	uas_up("call-ad1");
	rx(uas_in_dialog("INVITE", 2, NULL, sdp_body("0.0.0.0", 20000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(has(last_text(), "a=recvonly"));
	rx(uas_in_dialog("ACK", 2, NULL, NULL));
	CHECK_UINT(ev.hold_changed, 1);
	CHECK(ev.held);

	CASE("addr: the hold is a direction change and the stream stays up");
	CHECK_UINT(med.config_calls, 2);
	CHECK_UINT(med.last.direction, HAL_MEDIA_RECVONLY);
	CHECK_UINT(med.last.remote.ipver, 4);
	CHECK_UINT(med.last.remote.addr[0], 0);
	CHECK_UINT(med.last.remote.port, 20000);
	CHECK_UINT(med.stop_calls, 0);
	CHECK_UINT(ev.failed, 0);

	CASE("addr: and the resume takes the address back");
	rx(uas_in_dialog("INVITE", 3, NULL, sdp_body(T_PEER, 20000, "0", "a=sendrecv\r\n")));
	rx(uas_in_dialog("ACK", 3, NULL, NULL));
	CHECK_UINT(ev.hold_changed, 2);
	CHECK(!ev.held);
	CHECK_UINT(med.last.direction, HAL_MEDIA_SENDRECV);
	CHECK_UINT(med.last.remote.addr[0], 198);
}

/* ------------------------------------------------------------------ IPv6 */

/*
 * A line whose account view hands down an IPv6 address, on a backend that
 * carries IPv6 media. Both halves belong together: leg_media_take() refuses a
 * v6 leg on a backend that leaves HAL_FEAT_IPV6_MEDIA clear, because
 * stream_config_set rule 4 would refuse every description such a leg builds.
 */
static void v6_fx(void)
{
	fx_reset();
	fk.local_host = T_HOST6;
	calls_hal_features_set(HAL_FEAT_IPV6_MEDIA);
}

/*
 * One line in the IPv6 family. The cases cover the bracketed Contact of
 * RFC 3261 19.1.1. They cover the one-family rule of RFC 6157 section 5 at
 * the ring and at the description. They also cover the RFC 3605 RTCP endpoint
 * of the peer, and the backend gate.
 */
static void test_ipv6(void)
{
	struct cfg_channel ch = cfg.channel[0];
	struct sdp_result res;
	struct in6_addr want6;
	struct in_addr want4;
	struct hal_media m;
	struct req_info ri;

	CASE("contact: a v6 local host is bracketed in the INVITE Contact");
	v6_fx();
	CHECK(dial(0, 0, "1002"));
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.contact, "sip:1001@[" T_HOST6 "]:5060"));
	CHECK(has(send_text(0), "Contact: <sip:1001@[" T_HOST6 "]:5060>"));
	/* the leg keeps the bare host, so the Via writer brackets its own
	 * sent-by and the two spellings cannot drift apart */
	CHECK(has(send_text(0), "Via: SIP/2.0/UDP [" T_HOST6 "]:5060;"));

	CASE("contact: the 180 and the 200 of an inbound call are bracketed");
	v6_fx();
	rx(invite_text("call-v6-in", NULL,
		       sdp_body6(T_PEER6, 20000, "0 101", NULL)));
	CHECK(call_ring(0, 0));
	CHECK(has(send_text(0), "SIP/2.0 180"));
	CHECK(has(send_text(0), "Contact: <sip:1001@[" T_HOST6 "]:5060>"));
	CHECK(call_answer(0, 0));
	CHECK(has(send_text(1), "SIP/2.0 200"));
	CHECK(has(send_text(1), "Contact: <sip:1001@[" T_HOST6 "]:5060>"));

	CASE("contact: the REFER of a transfer is bracketed the same way");
	rx(uas_in_dialog("ACK", 1, NULL, NULL));
	CHECK(call_xfer(0, 0, CALL_XFER_BLIND, "1003"));
	CHECK(has(last_text(), "Contact: <sip:1001@[" T_HOST6 "]:5060>\r\n"));

	CASE("contact: a v4 local host keeps the shape it always had");
	fx_reset();
	CHECK(dial(0, 0, "1002"));
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.contact, "sip:1001@" T_HOST ":5060"));
	CHECK(!has(send_text(0), "@["));

	/*
	 * The account layer brackets a v6 domain before it builds the view, so
	 * this layer prints the text it receives and nothing here reads it.
	 */
	CASE("domain: a bracketed domain reaches the From, the To and the "
	     "request URI");
	fx_reset();
	fk.domain = T_DOMAIN6;
	CHECK(dial(0, 0, "1002"));
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.ruri, "sip:1002@" T_DOMAIN6));
	CHECK(has(ri.from, "sip:1001@" T_DOMAIN6));
	CHECK(has(ri.to, "sip:1002@" T_DOMAIN6));

	CASE("ring: a v6 offer on a v4 line is refused 488 before the ring");
	fx_reset();
	rx(invite_text("call-v6-fam", NULL,
		       sdp_body6(T_PEER6, 20000, "0 101", NULL)));
	CHECK(has(last_text(), "SIP/2.0 488"));
	CHECK_UINT(ev.incoming, 0);
	CHECK_UINT(sip_dialog_count(&dlgs), 0);
	CHECK(slot_is(0));

	CASE("ring: and the refusal reports that inbound leg exactly once");
	CHECK_UINT(ev.ended, 1);
	CHECK(ev.end.inbound);
	CHECK(!ev.end.answered);
	CHECK(!ev.end.media_ran);

	CASE("ring: a v4 offer on the same line still rings");
	fx_reset();
	rx(invite_text("call-v4-fam", NULL,
		       sdp_body(T_PEER, 20000, "0 101", NULL)));
	CHECK_UINT(ev.incoming, 1);
	CHECK(find_send("SIP/2.0 488") < 0);

	CASE("ring: a v6 offer on a v6 line rings and its stream runs in v6");
	v6_fx();
	rx(invite_text("call-v6-ok", NULL,
		       sdp_body6(T_PEER6, 20000, "0 101", NULL)));
	CHECK_UINT(ev.incoming, 1);
	CHECK(find_send("SIP/2.0 488") < 0);
	CHECK(call_answer(0, 0));
	CHECK_UINT(med.config_calls, 1);
	CHECK_UINT(med.last.local.ipver, 6);
	CHECK_UINT(med.last.remote.ipver, 6);

	memset(&res, 0, sizeof(res));
	res.codec.id = SDP_CODEC_PCMU;
	res.dtmf_pt = 101;
	res.cn_pt = -1;
	res.dir = SDP_SENDRECV;
	res.ptime_ms = 20;
	res.remote_port = 20000;
	res.remote_ipver = 6;
	res.valid = true;
	snprintf(res.remote_addr, sizeof(res.remote_addr), "%s", T_PEER6);

	CASE("fill: a v6 exchange gives ipver 6 and sixteen bytes at each end");
	media_fill(&ch, true, 5, false, &res, T_HOST6, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.local.ipver, 6);
	CHECK_UINT(m.remote.ipver, 6);
	CHECK_UINT(inet_pton(AF_INET6, T_HOST6, &want6), 1);
	CHECK_UINT(memcmp(m.local.addr, &want6, sizeof(want6)), 0);
	CHECK_UINT(inet_pton(AF_INET6, T_PEER6, &want6), 1);
	CHECK_UINT(memcmp(m.remote.addr, &want6, sizeof(want6)), 0);
	CHECK_UINT(m.local.port, 10000);
	CHECK_UINT(m.remote.port, 20000);
	CHECK_UINT(m.remote.rtcp_port, 20001);

	CASE("fill: a local and a remote of two families empty the record");
	media_fill(&ch, true, 5, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.local.ipver, 0);
	CHECK_UINT(m.remote.ipver, 0);
	CHECK_UINT(m.local.port, 0);
	CHECK_UINT(m.remote.port, 0);
	CHECK_UINT(m.codec, HAL_CODEC_NONE);

	/*
	 * The one wire path that reaches the fill with two families. A line
	 * whose own address is no literal names no family, so the SDP layer
	 * waives its half and takes the answer; the empty description then
	 * carries the refusal to media_program(), which is the refused-media
	 * class the leg already had for an address it cannot honor.
	 */
	CASE("fill: a mixed answer is refused as a media address");
	fx_reset();
	fk.local_host = "voice.example.net";
	CHECK(dial(0, 0, "1002"));
	reply(100, NULL, NULL, NULL);
	reply(200, "peer-tag", NULL, sdp_body6(T_PEER6, 20000, "0 101", NULL));
	CHECK_UINT(med.config_calls, 0);
	CHECK_UINT(med.start_calls, 0);
	CHECK_UINT(ev.answered, 0);
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.cls, CALL_FAIL_CONGESTION);
	CHECK(find_send("BYE") > 0);

	res.remote_ipver = 4;
	snprintf(res.remote_addr, sizeof(res.remote_addr), "%s", T_PEER);

	CASE("fill: the peer's a=rtcp port reaches the record without the bit");
	res.rtcp_port = 20010;
	media_fill(&ch, true, 5, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.remote.rtcp_port, 20010);

	CASE("fill: and with the bit, because every backend reads that port");
	media_fill(&ch, true, 5, true, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.remote.rtcp_port, 20010);

	CASE("fill: a section that named no port keeps the RTP port plus one");
	res.rtcp_port = 0;
	media_fill(&ch, true, 5, true, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.remote.rtcp_port, 20001);

	CASE("fill: the peer's RTCP address reaches a backend with the bit");
	res.rtcp_ipver = 4;
	snprintf(res.rtcp_addr, sizeof(res.rtcp_addr), "203.0.113.9");
	media_fill(&ch, true, 5, true, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.rtcp_remote.ipver, 4);
	CHECK_UINT(inet_pton(AF_INET, "203.0.113.9", &want4), 1);
	CHECK_UINT(memcmp(m.rtcp_remote.addr, &want4, sizeof(want4)), 0);

	CASE("fill: a backend without the bit leaves that field at zero");
	media_fill(&ch, true, 5, false, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.rtcp_remote.ipver, 0);
	CHECK_UINT(m.rtcp_remote.addr[0], 0);

	CASE("fill: a text the parse refuses leaves it at zero as well");
	snprintf(res.rtcp_addr, sizeof(res.rtcp_addr), "rtcp.example.net");
	media_fill(&ch, true, 5, true, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.rtcp_remote.ipver, 0);
	CHECK_UINT(m.rtcp_remote.addr[0], 0);

	CASE("fill: and so does a peer that named a port alone");
	res.rtcp_ipver = 0;
	res.rtcp_addr[0] = 0;
	media_fill(&ch, true, 5, true, &res, T_HOST, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.rtcp_remote.ipver, 0);
	CHECK_UINT(m.rtcp_remote.addr[0], 0);

	CASE("fill: a v6 RTCP address carries ipver 6 and its sixteen bytes");
	res.remote_ipver = 6;
	snprintf(res.remote_addr, sizeof(res.remote_addr), "%s", T_PEER6);
	res.rtcp_ipver = 6;
	snprintf(res.rtcp_addr, sizeof(res.rtcp_addr), "2001:db8::a");
	media_fill(&ch, true, 5, true, &res, T_HOST6, 10000, "wan",
		   HAL_PASSTHROUGH_OFF, 0, &m);
	CHECK_UINT(m.rtcp_remote.ipver, 6);
	CHECK_UINT(inet_pton(AF_INET6, "2001:db8::a", &want6), 1);
	CHECK_UINT(memcmp(m.rtcp_remote.addr, &want6, sizeof(want6)), 0);

	/*
	 * The one place a backend feature turns into the fill's rtcp_addr
	 * argument. The cases above hand that argument in by value, so they say
	 * nothing about the expression media_program() derives from
	 * cv.hal_features; these two drive a whole call instead and read the
	 * description the fake HAL received.
	 */
	CASE("program: the bit carries the peer's a=rtcp address to the HAL");
	fx_reset();
	calls_hal_features_set(HAL_FEAT_RTCP_ADDR);
	CHECK(dial(0, 0, "1002"));
	reply(100, NULL, NULL, NULL);
	reply(200, "peer-tag", NULL,
	      sdp_body(T_PEER, 20000, "0 101",
		       "a=rtcp:20010 IN IP4 203.0.113.9\r\n"));
	CHECK_UINT(ev.answered, 1);
	CHECK_UINT(med.config_calls, 1);
	CHECK_UINT(med.last.remote.rtcp_port, 20010);
	CHECK_UINT(med.last.rtcp_remote.ipver, 4);
	CHECK_UINT(inet_pton(AF_INET, "203.0.113.9", &want4), 1);
	CHECK_UINT(memcmp(med.last.rtcp_remote.addr, &want4, sizeof(want4)), 0);

	CASE("program: without the bit the same call carries the port alone");
	fx_reset();
	CHECK(dial(0, 0, "1002"));
	reply(100, NULL, NULL, NULL);
	reply(200, "peer-tag", NULL,
	      sdp_body(T_PEER, 20000, "0 101",
		       "a=rtcp:20010 IN IP4 203.0.113.9\r\n"));
	CHECK_UINT(ev.answered, 1);
	CHECK_UINT(med.config_calls, 1);
	CHECK_UINT(med.last.remote.rtcp_port, 20010);
	CHECK_UINT(med.last.rtcp_remote.ipver, 0);
	CHECK_UINT(med.last.rtcp_remote.addr[0], 0);

	CASE("take: a v6 line on a backend without the bit hears congestion");
	fx_reset();
	fk.local_host = T_HOST6;
	CHECK(!call_dial(0, 0, "1002", true));
	CHECK_UINT(fk.sends, 0);
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.cls, CALL_FAIL_CONGESTION);
	CHECK(slot_is(0));

	CASE("take: and an inbound INVITE on that line is answered 486");
	fx_reset();
	fk.local_host = T_HOST6;
	rx(invite_text("call-v6-off", NULL,
		       sdp_body6(T_PEER6, 20000, "0 101", NULL)));
	CHECK(has(last_text(), "SIP/2.0 486"));
	CHECK_UINT(ev.incoming, 0);
	CHECK_UINT(med.config_calls, 0);
	CHECK(slot_is(0));

	CASE("take: the same line dials once the backend carries v6 media");
	v6_fx();
	CHECK(dial(0, 0, "1002"));
	CHECK_UINT(fk.sends, 1);
	CHECK(last_req(&ri));
	CHECK(has(ri.body, "c=IN IP6 " T_HOST6));
}

static void test_ended(void)
{
	CASE("ended: a peer BYE reports the leg once");
	fx_reset();
	uas_up("call-en1");
	rx(uas_in_dialog("BYE", 2, NULL, NULL));
	CHECK_UINT(ev.ended, 1);
	CHECK_UINT(ev.end.line, 0);
	CHECK_UINT(ev.end.slot, 0);

	CASE("ended: an answered call that nothing failed says exactly that");
	CHECK(ev.end.inbound);
	CHECK(ev.end.answered);
	CHECK(!ev.end.failed);
	CHECK(ev.end.media_ran);

	CASE("ended: the snapshot is taken BEFORE the stream stops");
	CHECK_UINT(med.snapshot_calls, 1);
	CHECK(med_seq_index(MED_SNAPSHOT) >= 0);
	CHECK(med_seq_index(MED_SNAPSHOT) < med_seq_index(MED_STOP));

	CASE("ended: the snapshot names the line and the slot as the stream");
	CHECK_UINT(med.snapshot_line, 0);
	CHECK_UINT(med.snapshot_stream, 0);

	CASE("ended: the report is made after the stop it belongs to");
	CHECK_UINT(ev.end.stops, 1);

	CASE("ended: it carries the committed codec, its ptime and the rtp port");
	CHECK_UINT(ev.end.codec, HAL_CODEC_PCMU);
	CHECK_UINT(ev.end.ptime_ms, 20);
	CHECK(ev.end.rtp_port != 0);

	CASE("ended: an inbound leg carries the caller identity, not a dialled number");
	CHECK(!strcmp(ev.end.remote_number, "1002"));
	CHECK(!strcmp(ev.end.remote_name, "Alice"));
	CHECK(!strcmp(ev.end.number, ""));

	CASE("ended: a local hangup waits for the BYE transaction to end");
	fx_reset();
	uac_up(NULL);
	CHECK(call_hangup(0, 0));
	CHECK_UINT(ev.ended, 0);
	CHECK_UINT(med.snapshot_calls, 1);
	CHECK(med_seq_index(MED_SNAPSHOT) < med_seq_index(MED_STOP));

	CASE("ended: and then reports once, answered and not failed");
	reply(200, NULL, NULL, NULL);
	fake_advance(6000);
	CHECK_UINT(ev.ended, 1);
	CHECK(!ev.end.inbound);
	CHECK(ev.end.answered);
	CHECK(!ev.end.failed);
	CHECK(!strcmp(ev.end.number, "1002"));

	/* The daemon stop ends the legs before the backend closes, a leg that
	 * still waits for its BYE answer included. */
	CASE("ended: the stop of a line ends a leg that waits for its BYE "
	     "answer");
	fx_reset();
	uac_up(NULL);
	CHECK(call_hangup(0, 0));
	CHECK_UINT(ev.ended, 0);
	CHECK(!slot_is(0));
	calls_line_end(0);
	CHECK_UINT(ev.ended, 1);
	CHECK(slot_is(0));

	/* The record names the codec in the HAL vocabulary (proto_media.h
	 * struct proto_call_ended), which differs from the SDP one past
	 * G.729. */
	CASE("ended: a wideband leg reports its codec as the HAL names it");
	cfg_default();
	fx_reset();
	codec_sections_set(5, 0);
	uac_up_codec("9 101", "a=rtpmap:9 G722/8000\r\n");
	CHECK_UINT(med.last.codec, HAL_CODEC_G722);
	CHECK(call_hangup(0, 0));
	reply(200, NULL, NULL, NULL);
	fake_advance(6000);
	CHECK_UINT(ev.ended, 1);
	CHECK_UINT(ev.end.codec, HAL_CODEC_G722);
	cfg_default();

	CASE("ended: a failed setup carries the clearing class the FSM was told");
	fx_reset();
	CHECK(dial(0, 0, "1002"));
	reply(486, "peer-tag", NULL, NULL);
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.ended, 1);
	CHECK(ev.end.failed);
	CHECK_UINT(ev.end.cls, CALL_FAIL_BUSY);
	CHECK(!ev.end.answered);

	CASE("ended: a call whose media never ran takes no snapshot");
	CHECK(!ev.end.media_ran);
	CHECK_UINT(med.snapshot_calls, 0);
	CHECK_UINT(med.stop_calls, 0);

	/* the record names the cause the caller was sent, and the machine still
	 * hears nothing: it asked for the rejection itself */
	CASE("ended: a rejected offer reports the leg once, with its cause");
	fx_reset();
	rx(invite_text("call-en2", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(call_reject(0, 0, 486));
	CHECK_UINT(ev.ended, 1);
	CHECK(ev.end.inbound);
	CHECK(!ev.end.answered);
	CHECK(ev.end.failed);
	CHECK_UINT(ev.end.cls, CALL_FAIL_BUSY);
	CHECK_UINT(ev.failed, 0);
	CHECK_UINT(med.snapshot_calls, 0);

	CASE("ended: a forwarded offer reports the leg once");
	fx_reset();
	rx(invite_text("call-en3", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(call_forward(0, 0, "1003"));
	CHECK_UINT(ev.ended, 1);
	CHECK(ev.end.inbound);
	CHECK(!ev.end.answered);

	CASE("ended: a peer CANCEL reports the leg once");
	fx_reset();
	rx(invite_text("call-en4", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(call_ring(0, 0));
	rx(cancel_text("call-en4"));
	CHECK_UINT(ev.remote_cancel, 1);
	CHECK_UINT(ev.ended, 1);
	CHECK(!ev.end.answered);
	CHECK(!ev.end.failed);

	CASE("ended: our own CANCEL reports the leg once, at the 487");
	fx_reset();
	CHECK(dial(0, 0, "1002"));
	reply(100, NULL, NULL, NULL);
	CHECK(call_hangup(0, 0));
	CHECK(find_send("CANCEL") > 0);
	CHECK_UINT(ev.ended, 0);
	reply_at(0, 487, "peer-tag", NULL, NULL);
	CHECK_UINT(ev.ended, 1);
	CHECK(!ev.end.inbound);
	CHECK(!ev.end.answered);

	/*
	 * The one path that stops media inside the release. Every other
	 * clearing path stops the stream on its way out (a BYE, a peer BYE), so
	 * only a leg that started early media and was then cancelled reaches
	 * leg_release() with a running stream. It is what pins the order of the
	 * release itself: the snapshot goes with the stop, and the report comes
	 * after both, or the record would close before its media was asked for.
	 */
	CASE("ended: a cancelled early media leg stops its stream in the release");
	fx_reset();
	cfg.channel[0].early_media_enabled = true;
	CHECK(dial(0, 0, "1002"));
	reply(183, "peer-tag", NULL, sdp_body(T_PEER, 20000, "0 101", NULL));
	CHECK_UINT(med.start_calls, 1);
	CHECK(call_hangup(0, 0));
	CHECK(find_send("CANCEL") > 0);
	CHECK_UINT(med.stop_calls, 0);
	CHECK_UINT(ev.ended, 0);

	CASE("ended: and the snapshot still stands in front of that stop");
	reply_at(0, 487, "peer-tag", NULL, NULL);
	CHECK_UINT(ev.ended, 1);
	CHECK_UINT(med.snapshot_calls, 1);
	CHECK_UINT(med.stop_calls, 1);
	CHECK(med_seq_index(MED_SNAPSHOT) < med_seq_index(MED_STOP));

	CASE("ended: the report of that leg is made after its stop");
	CHECK_UINT(ev.end.stops, 1);
	CHECK(ev.end.media_ran);
	CHECK(!ev.end.answered);
	cfg.channel[0].early_media_enabled = false;

	CASE("ended: a line teardown of an offered call reports it once");
	fx_reset();
	rx(invite_text("call-en5", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	calls_line_teardown(0);
	CHECK(has(last_text(), "SIP/2.0 480"));
	CHECK_UINT(ev.ended, 1);
	CHECK(ev.end.inbound);

	CASE("ended: a line teardown of an answered call reports it with its BYE");
	fx_reset();
	uas_up("call-en6");
	calls_line_teardown(0);
	CHECK(find_send("BYE") > 0);
	CHECK_UINT(med.snapshot_calls, 1);
	CHECK(med_seq_index(MED_SNAPSHOT) < med_seq_index(MED_STOP));
	reply(200, NULL, NULL, NULL);
	fake_advance(6000);
	CHECK_UINT(ev.ended, 1);
	CHECK(ev.end.answered);

	CASE("ended: a draining leg reclaimed for a new call is reported first");
	fx_reset();
	uac_up(NULL);
	CHECK(call_hangup(0, 0));
	CHECK_UINT(ev.ended, 0);
	CHECK(dial(0, 0, "1003"));
	CHECK_UINT(ev.ended, 1);
	CHECK(ev.end.answered);
	CHECK(!strcmp(ev.end.number, "1002"));

	CASE("ended: the new leg is reported on its own end");
	reply(486, "peer-tag", NULL, NULL);
	CHECK_UINT(ev.ended, 2);
	CHECK(!strcmp(ev.end.number, "1003"));
	CHECK(ev.end.failed);

	CASE("ended: an offer refused for want of media reports the leg once");
	fx_reset();
	cfg.codecs = 1;
	memset(&cfg.codec[0], 0, sizeof(cfg.codec[0]));
	cfg.codec[0].channel = 1;		/* channel id 0, and disabled */
	rx(invite_text("call-en7", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 486"));
	CHECK_UINT(ev.incoming, 0);
	CHECK_UINT(ev.ended, 1);
	CHECK(ev.end.inbound);
	CHECK(!ev.end.media_ran);
	cfg.codecs = 0;

	CASE("ended: an offer with a body we cannot parse reports the leg once");
	fx_reset();
	rx(invite_text("call-en8", NULL, "this is not an sdp body"));
	CHECK(has(last_text(), "SIP/2.0 488"));
	CHECK_UINT(ev.incoming, 0);
	CHECK_UINT(ev.ended, 1);
	CHECK(ev.end.inbound);

	CASE("ended: a slots-busy refusal creates no leg and reports nothing");
	fx_reset();
	uas_up("call-en9");
	rx(invite_text("call-en10", NULL, sdp_body(T_PEER, 20001, "0", NULL)));
	rx(invite_text("call-en11", NULL, sdp_body(T_PEER, 20002, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 486"));
	CHECK_UINT(ev.ended, 0);

	/*
	 * The stream index is the slot, and every case above drives slot 0,
	 * where a snapshot that named stream 0 by hand would answer exactly the
	 * same. A second leg on one line is the only shape that separates the
	 * two, and it is the shape a waiting call takes.
	 */
	CASE("ended: a second leg on one line answers on its own slot");
	fx_reset();
	uas_up("call-en13");
	rx(invite_text("call-en14", NULL, sdp_body(T_PEER, 20002, "0 101", NULL)));
	CHECK_UINT(ev.incoming, 2);
	CHECK_UINT(ev.slot, 1);
	CHECK(call_ring(0, 1));
	CHECK(call_answer(0, 1));
	rx(uas_in_dialog("ACK", 1, NULL, NULL));
	CHECK_UINT(med.start_calls, 2);
	CHECK_UINT(med.last_stream, 1);

	CASE("ended: the snapshot of that leg names stream 1, not stream 0");
	rx(uas_in_dialog("BYE", 2, NULL, NULL));
	CHECK_UINT(ev.ended, 1);
	CHECK_UINT(ev.end.slot, 1);
	CHECK_UINT(med.snapshot_calls, 1);
	CHECK_UINT(med.snapshot_line, 0);
	CHECK_UINT(med.snapshot_stream, 1);

	CASE("ended: and the leg on slot 0 kept its own stream running");
	CHECK_UINT(med.stop_calls, 1);
	CHECK(slot_is(1));

	CASE("ended: a re-INVITE does not report an end of its own");
	fx_reset();
	uas_up("call-en12");
	rx(uas_in_dialog("INVITE", 2, NULL,
			 sdp_body(T_PEER, 20000, "0 101", "a=sendonly\r\n")));
	rx(uas_in_dialog("ACK", 2, NULL, NULL));
	CHECK_UINT(ev.ended, 0);

	/* the whole shutdown story is test_shutdown's; this is the one leg the
	 * case above left up, and the stop owes it a report like any other */
	CASE("ended: a call still up at the shutdown teardown is reported");
	calls_done();
	CHECK_UINT(ev.ended, 1);
	CHECK_UINT(count_send("BYE"), 1);
}

/*
 * The clear that the report carries. A BYE-cleared leg is released at one of
 * two distances from its BYE. It drains its BYE transaction for timer K, or a
 * successor takes the slot back at once. The report closes the gap between
 * the two endings of one call. Both cases below therefore rebuild the clear
 * point from the report, and compare it with the BYE.
 */
static void test_cleared(void)
{
	uint64_t bye_at;

	CASE("cleared: a hangup reports the clear, aged by the BYE drain");
	fx_reset();
	uac_up(NULL);
	bye_at = fk.now;
	CHECK(call_hangup(0, 0));
	CHECK_UINT(ev.ended, 0);
	fake_advance(2000);
	reply(200, NULL, NULL, NULL);
	fake_advance(6000);
	CHECK_UINT(ev.ended, 1);
	CHECK(ev.end.cleared);
	CHECK(ev.end.cleared_ago_ms >= 2000);

	CASE("cleared: and the clear point it names is the BYE, not the release");
	CHECK(ev.end.at_ms > bye_at);
	CHECK_UINT(ev.end.at_ms - ev.end.cleared_ago_ms, bye_at);

	CASE("cleared: a successor that reclaims the slot names the same point");
	fx_reset();
	uac_up(NULL);
	bye_at = fk.now;
	CHECK(call_hangup(0, 0));
	CHECK_UINT(ev.ended, 0);
	fake_advance(120);
	CHECK(dial(0, 0, "1003"));
	CHECK_UINT(ev.ended, 1);
	CHECK(ev.end.cleared);
	CHECK_UINT(ev.end.cleared_ago_ms, 120);
	CHECK_UINT(ev.end.at_ms - ev.end.cleared_ago_ms, bye_at);

	CASE("cleared: the successor leg carries no clear of its own");
	reply(486, "peer-tag", NULL, NULL);
	CHECK_UINT(ev.ended, 2);
	CHECK(!ev.end.cleared);
	CHECK_UINT(ev.end.cleared_ago_ms, 0);

	CASE("cleared: a peer BYE clears the leg where it arrives");
	fx_reset();
	uas_up("call-cl1");
	fake_advance(3000);
	rx(uas_in_dialog("BYE", 2, NULL, NULL));
	CHECK_UINT(ev.ended, 1);
	CHECK(ev.end.cleared);
	CHECK_UINT(ev.end.cleared_ago_ms, 0);

	CASE("cleared: a peer BYE in an early dialog clears it too");
	fx_reset();
	CHECK(dial(0, 0, "1002"));
	reply(100, NULL, NULL, NULL);
	reply(180, "peer-tag", NULL, NULL);
	rx(uac_in_dialog("BYE", "peer-tag", 1, NULL, NULL));
	CHECK_UINT(ev.ended, 1);
	CHECK_UINT(ev.failed, 1);
	CHECK(ev.end.cleared);
	CHECK_UINT(ev.end.cleared_ago_ms, 0);

	/*
	 * Both sides hung up at once. rx_in_dialog() dispatches a BYE by its
	 * method alone, so a peer BYE reaches a leg that is already draining one
	 * of its own, and a second stamp would move the end of the call forward
	 * by the whole glare interval. The call ended at the first clear.
	 */
	CASE("cleared: a peer BYE into our draining leg keeps the first clear");
	fx_reset();
	uac_up(NULL);
	bye_at = fk.now;
	CHECK(call_hangup(0, 0));
	CHECK_UINT(ev.ended, 0);
	fake_advance(700);
	rx(uac_in_dialog("BYE", "peer-tag", 2, NULL, NULL));
	CHECK_UINT(ev.ended, 1);
	CHECK(ev.end.cleared);
	CHECK_UINT(ev.end.cleared_ago_ms, 700);
	CHECK_UINT(ev.end.at_ms - ev.end.cleared_ago_ms, bye_at);

	CASE("cleared: a busy final ends the leg with no clear at all");
	fx_reset();
	CHECK(dial(0, 0, "1002"));
	reply(486, "peer-tag", NULL, NULL);
	CHECK_UINT(ev.ended, 1);
	CHECK(ev.end.failed);
	CHECK(!ev.end.cleared);
	CHECK_UINT(ev.end.cleared_ago_ms, 0);

	CASE("cleared: our own CANCEL ends it with no clear either");
	fx_reset();
	CHECK(dial(0, 0, "1002"));
	reply(100, NULL, NULL, NULL);
	CHECK(call_hangup(0, 0));
	reply_at(0, 487, "peer-tag", NULL, NULL);
	CHECK_UINT(ev.ended, 1);
	CHECK(!ev.end.cleared);

	CASE("cleared: a BYE the transport refused still ends the call at the BYE");
	fx_reset();
	uac_up(NULL);
	bye_at = fk.now;
	fk.send_fail = true;
	CHECK(call_hangup(0, 0));
	fk.send_fail = false;
	CHECK(find_send("BYE") < 0);
	CHECK_UINT(ev.ended, 1);
	CHECK(ev.end.cleared);
	CHECK_UINT(ev.end.cleared_ago_ms, 0);
	CHECK_UINT(ev.end.at_ms, bye_at);
}

/*
 * A peer re-INVITE that arrives behind our own BYE names a dialog that this
 * side does not serve any more (RFC 3261 15.1.1). It is answered 481, not the
 * 491 that a running exchange of ours earns. Reason: a 491 promises a retry
 * that the dialog will never take.
 */
static void test_bye_sent_reinvite(void)
{
	CASE("bye-sent: a peer re-INVITE after our BYE is answered 481, not 491");
	fx_reset();
	uac_up(NULL);
	CHECK(call_hangup(0, 0));
	CHECK_UINT(count_send("BYE"), 1);
	rx(uac_in_dialog("INVITE", "peer-tag", 1, NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 481"));

	CASE("bye-sent: a glare re-INVITE during our own exchange still gets 491");
	fx_reset();
	uac_up(NULL);
	CHECK(call_hold(0, 0));
	rx(uac_in_dialog("INVITE", "peer-tag", 1, NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 491"));
}

/*
 * The daemon stop closes every call. calls_line_teardown() leaves a leg alive
 * while its BYE or CANCEL is on the wire, so that the leg can finish it.
 * Reason: the reload path drains that leg and tears the line down a second
 * time. A stop runs neither, so calls_done() ends what the teardown left.
 * Every leg that was in use reports its end exactly once. The suite watches
 * the callback, which is where line/line.c closes the record.
 */
static void test_shutdown(void)
{
	CASE("shutdown: one stop ends a connected leg, a ringing one and an early dialog");
	fx_reset();
	uas_up("call-sd1");
	rx(invite_text("call-sd2", NULL, sdp_body(T_PEER, 20002, "0", NULL)));
	CHECK(call_ring(0, 1));
	CHECK(dial(1, 0, "1003"));
	reply(100, NULL, NULL, NULL);
	reply(180, "peer-tag", NULL, NULL);
	CHECK_UINT(ev.ended, 0);
	calls_done();
	CHECK_UINT(ev.ends[0][0].count, 1);
	CHECK_UINT(ev.ends[0][1].count, 1);
	CHECK_UINT(ev.ends[1][0].count, 1);

	/*
	 * The report count is what pins the order: the wipe at the end of
	 * calls_done() takes the callback table with it, so a report made after
	 * it would reach nobody and every count above would read zero.
	 */
	CASE("shutdown: the reports are made before the state wipe");
	CHECK_UINT(ev.ended, 3);

	CASE("shutdown: the connected leg carries the clear of the teardown BYE");
	CHECK_UINT(count_send("BYE"), 1);
	CHECK_UINT(ev.ends[0][0].count, 1);
	CHECK(ev.ends[0][0].answered);
	CHECK(ev.ends[0][0].cleared);

	CASE("shutdown: the ringing leg is rejected instead, so nothing cleared it");
	CHECK(find_send("SIP/2.0 480") >= 0);
	CHECK_UINT(ev.ends[0][1].count, 1);
	CHECK(ev.ends[0][1].inbound);
	CHECK(!ev.ends[0][1].answered);
	CHECK(!ev.ends[0][1].cleared);

	CASE("shutdown: the early dialog is cancelled, and no BYE clears that one either");
	CHECK_UINT(count_send("CANCEL"), 1);
	CHECK_UINT(ev.ends[1][0].count, 1);
	CHECK(!ev.ends[1][0].inbound);
	CHECK(!ev.ends[1][0].answered);
	CHECK(!ev.ends[1][0].cleared);

	/*
	 * Both slots of one line, and both answered, so both reach the stop half
	 * of the teardown. The ringing leg above never does: call_reject() ends
	 * it inside calls_line_teardown(), so a stop that walked slot 0 alone
	 * would still pass, and a waiting call's record would stay open.
	 */
	CASE("shutdown: two answered legs of one line each report on their own slot");
	fx_reset();
	uas_up("call-sd4");
	rx(invite_text("call-sd5", NULL, sdp_body(T_PEER, 20004, "0 101", NULL)));
	CHECK(call_ring(0, 1));
	CHECK(call_answer(0, 1));
	rx(uas_in_dialog("ACK", 1, NULL, NULL));
	calls_done();
	CHECK_UINT(ev.ended, 2);
	CHECK_UINT(ev.ends[0][0].count, 1);
	CHECK_UINT(ev.ends[0][1].count, 1);
	CHECK_UINT(count_send("BYE"), 2);
	CHECK(ev.ends[0][0].cleared);
	CHECK(ev.ends[0][1].cleared);

	CASE("shutdown: an INVITE that drew no provisional needs no CANCEL to end");
	fx_reset();
	CHECK(dial(0, 0, "1002"));
	calls_done();
	CHECK_UINT(ev.ended, 1);
	CHECK_UINT(count_send("CANCEL"), 0);
	CHECK(!ev.ends[0][0].cleared);

	/* the truthful duration of a stop: the call ended at the BYE, not at the
	 * release the stop forces two seconds later */
	CASE("shutdown: a leg draining its own hangup BYE ends at that BYE");
	fx_reset();
	uac_up(NULL);
	CHECK(call_hangup(0, 0));
	CHECK_UINT(ev.ended, 0);
	fake_advance(2000);
	calls_done();
	CHECK_UINT(ev.ended, 1);
	CHECK(ev.end.cleared);
	CHECK(ev.end.cleared_ago_ms >= 2000);

	CASE("shutdown: a leg that ended before the stop is not reported twice");
	fx_reset();
	uas_up("call-sd3");
	rx(uas_in_dialog("BYE", 2, NULL, NULL));
	CHECK_UINT(ev.ended, 1);
	calls_done();
	CHECK_UINT(ev.ended, 1);
	CHECK_UINT(ev.ends[0][0].count, 1);

	CASE("shutdown: a stop with no leg in use reports nothing");
	fx_reset();
	calls_done();
	CHECK_UINT(ev.ended, 0);
}

/* ---------------------------------------------------- the account transport */

/*
 * The transport token that the view hands over. It reaches the Via of every
 * request that the leg builds. It also reaches the URI of the Contact of the
 * leg itself, in lower case there (account.h). A view that names no transport
 * keeps the UDP wire byte for byte, which every case above this one checks.
 */
static void test_transport(void)
{
	struct req_info ri, inv;
	unsigned int before;
	int ridx;

	fx_reset();
	fk.transport = "TLS";

	CASE("transport: the outbound INVITE Via names the account transport");
	CHECK(dial(0, 0, "1002"));
	CHECK(has(send_text(0), "Via: SIP/2.0/TLS " T_HOST ":5060;"));

	CASE("transport: the INVITE Contact carries the parameter inside the brackets");
	CHECK(has(send_text(0), "Contact: <sip:1001@" T_HOST ":5060;transport=tls>"));

	CASE("transport: the ACK of the 2xx repeats the token in its Via");
	reply(100, NULL, NULL, NULL);
	reply(200, "peer-tag", NULL, sdp_body(T_PEER, 20000, "0 101", NULL));
	ridx = find_send("ACK");
	CHECK(ridx >= 0);
	CHECK(has(send_text((unsigned int)ridx), "Via: SIP/2.0/TLS " T_HOST ":5060;"));

	CASE("transport: the in-dialog BYE repeats the token in its Via");
	CHECK(call_hangup(0, 0));
	ridx = find_send("BYE");
	CHECK(ridx >= 0);
	CHECK(has(send_text((unsigned int)ridx), "Via: SIP/2.0/TLS " T_HOST ":5060;"));

	CASE("transport: a TCP account spells both places for TCP");
	fx_reset();
	fk.transport = "TCP";
	CHECK(dial(0, 0, "1002"));
	CHECK(has(send_text(0), "Via: SIP/2.0/TCP " T_HOST ":5060;"));
	CHECK(has(send_text(0), "Contact: <sip:1001@" T_HOST ":5060;transport=tcp>"));

	CASE("transport: the provisional and the 2xx carry the parameter too");
	fx_reset();
	fk.transport = "TLS";
	uas_up("call-tp1");
	CHECK(has(send_text(0), "SIP/2.0 180"));
	CHECK(has(send_text(0), "Contact: <sip:1001@" T_HOST ":5060;transport=tls>"));
	CHECK(has(send_text(1), "SIP/2.0 200"));
	CHECK(has(send_text(1), "Contact: <sip:1001@" T_HOST ":5060;transport=tls>"));

	/* the redirect Contact is the forward target, not our own address, so
	 * the transport of this line says nothing about how to reach it */
	CASE("transport: a redirect Contact stays the target's, undecorated");
	fx_reset();
	fk.transport = "TLS";
	rx(invite_text("call-tp2", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(call_forward(0, 0, "5551234"));
	CHECK(has(last_text(), "SIP/2.0 302"));
	CHECK(has(last_text(), "Contact: <sip:5551234@" T_DOMAIN ">"));
	CHECK(!has(last_text(), "transport="));

	CASE("transport: a view that names none keeps the UDP wire");
	fx_reset();
	CHECK(dial(0, 0, "1002"));
	CHECK(has(send_text(0), "Via: SIP/2.0/UDP " T_HOST ":5060;"));
	CHECK(has(send_text(0), "Contact: <sip:1001@" T_HOST ":5060>"));
	CHECK(!has(send_text(0), "transport="));

	/*
	 * The CANCEL (RFC 3261 9.1) and the ACK of a non-2xx (17.1.1.3) re-emit
	 * the Via of the retained INVITE, so the token rides along with the
	 * branch the transaction matches on. A rebuild of that Via would keep
	 * every case above this one green.
	 */
	CASE("transport: the CANCEL and the ACK of a non-2xx repeat the INVITE Via");
	fx_reset();
	fk.transport = "TLS";
	CHECK(dial(0, 0, "1002"));
	CHECK(req_info_at(0, &inv));
	reply(100, NULL, NULL, NULL);
	CHECK(call_hangup(0, 0));
	ridx = find_send("CANCEL");
	CHECK(ridx >= 0);
	CHECK(has(send_text((unsigned int)ridx), "Via: SIP/2.0/TLS " T_HOST ":5060;"));
	CHECK(req_info_at((unsigned int)ridx, &ri));
	CHECK(!strcmp(ri.branch, inv.branch));
	reply_at(0, 487, "peer-tag", NULL, NULL);
	ridx = find_send("ACK");
	CHECK(ridx >= 0);
	CHECK(has(send_text((unsigned int)ridx), "Via: SIP/2.0/TLS " T_HOST ":5060;"));
	CHECK(req_info_at((unsigned int)ridx, &ri));
	CHECK(!strcmp(ri.branch, inv.branch));

	/* the token dies with the leg that took it, not with the process */
	CASE("transport: a recycled leg drops the token with the account that named it");
	fx_reset();
	fk.transport = "TLS";
	CHECK(dial(0, 0, "1002"));
	CHECK(call_hangup(0, 0));
	fake_advance(40000);
	CHECK(slot_is(0));
	fk.transport = NULL;
	before = fk.sends;
	CHECK(dial(0, 0, "1003"));
	CHECK(has(send_text(before), "Via: SIP/2.0/UDP " T_HOST ":5060;"));
	CHECK(has(send_text(before), "Contact: <sip:1001@" T_HOST ":5060>"));
	CHECK(!has(send_text(before), "transport="));
}

/* the two headers RFC 3262 3 puts on a reliable provisional response */
static const char *rel_hdrs(uint32_t rseq, const char *more)
{
	char *buf = text_slot();

	snprintf(buf, TEXT_MAX, "Require: 100rel\r\nRSeq: %u\r\n%s", rseq,
		 more ? more : "");

	return buf;
}

/*
 * A provisional response to the INVITE of this leg, whatever the leg put on
 * the wire since: reply() answers the last request, which is the PRACK once
 * one is out.
 */
static void prov_at_invite(uint32_t code, const char *to_tag, const char *extra,
			   const char *body)
{
	int idx = find_send("INVITE");

	CHECK(idx >= 0);
	if (idx >= 0)
		reply_at((unsigned int)idx, code, to_tag, extra, body);
}

static void rel_prov(uint32_t code, uint32_t rseq, const char *to_tag,
		     const char *more, const char *body)
{
	prov_at_invite(code, to_tag, rel_hdrs(rseq, more), body);
}

/* the text of the last PRACK that went out */
static const char *prack_text(void)
{
	int idx = find_send("PRACK");

	return idx >= 0 ? send_text((unsigned int)idx) : "";
}

/*
 * The UAC half of RFC 3262. One PRACK answers every reliable provisional,
 * and a retransmission and a gap get no PRACK. An answer that a reliable 1xx
 * carried is the one answer of the exchange (RFC 3261 13.2.1).
 */
static void test_prack_uac(void)
{
	const char *chal = "WWW-Authenticate: Digest realm=\"" T_DOMAIN
			   "\", nonce=\"" T_NONCE "\"\r\n";
	/* a second nonce: the auth state refuses the same one twice, so a case
	 * that pins a retry needs a challenge the leg has not seen */
	const char *chal_b = "WWW-Authenticate: Digest realm=\"" T_DOMAIN
			     "\", nonce=\"5e2d9a41c0b7638f\"\r\n";
	const char *contact_b = "Contact: <sip:1002@" T_PEER ":5061>\r\n";
	const char *rr = "Record-Route: <sip:proxy.example.net;lr>\r\n";
	struct req_info ri, inv;
	int idx;

	CASE("prack uac: a reliable 180 draws one PRACK inside the new dialog");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	CHECK(req_info_at(0, &inv));
	reply(100, NULL, NULL, NULL);
	rel_prov(180, 10, "peer-tag", rr, NULL);
	CHECK_UINT(count_send("PRACK"), 1);
	CHECK(req_of("PRACK", &ri));
	CHECK(!strcmp(ri.ruri, "sip:1002@" T_PEER ":5060"));
	CHECK_UINT(ri.cseq, 2);
	CHECK(!strcmp(ri.call_id, inv.call_id));
	CHECK(!strcmp(ri.from_tag, inv.from_tag));
	CHECK(!strcmp(ri.to_tag, "peer-tag"));
	CHECK_UINT(tr.prack_sent_lines, 1);
	CHECK_UINT(ev.proceeding, 1);

	CASE("prack uac: the RAck names the RSeq and the CSeq of the 1xx");
	CHECK(has(prack_text(), "RAck: 10 1 INVITE\r\n"));
	CHECK(has(prack_text(), "Route: <sip:proxy.example.net;lr>"));
	CHECK(!has(prack_text(), "\r\nContact:"));

	CASE("prack uac: the 200 to the PRACK is absorbed");
	reply(200, NULL, NULL, NULL);
	CHECK_UINT(ev.answered, 0);
	CHECK_UINT(ev.failed, 0);
	reply_at(0, 200, "peer-tag", NULL,
		 sdp_body(T_PEER, 20000, "0 101", NULL));
	CHECK_UINT(ev.answered, 1);

	CASE("prack uac: a retransmitted reliable 180 draws no second PRACK");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	rel_prov(180, 10, "peer-tag", NULL, NULL);
	CHECK_UINT(count_send("PRACK"), 1);
	rel_prov(180, 10, "peer-tag", NULL, NULL);
	CHECK_UINT(count_send("PRACK"), 1);
	CHECK_UINT(ev.proceeding, 1);
	CHECK_UINT(tr.prack_retx_lines, 1);

	CASE("prack uac: an RSeq gap waits for the number it missed");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	rel_prov(183, 10, "peer-tag", NULL, NULL);
	CHECK_UINT(count_send("PRACK"), 1);
	rel_prov(180, 12, "peer-tag", NULL, NULL);
	CHECK_UINT(count_send("PRACK"), 1);
	CHECK_UINT(tr.prack_order_lines, 1);
	rel_prov(180, 11, "peer-tag", NULL, NULL);
	CHECK_UINT(count_send("PRACK"), 2);
	CHECK(has(prack_text(), "RAck: 11 1 INVITE\r\n"));
	CHECK(req_of("PRACK", &ri));
	CHECK_UINT(ri.cseq, 3);

	CASE("prack uac: an RSeq without Require is a plain 1xx");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	prov_at_invite(180, "peer-tag", "RSeq: 10\r\n", NULL);
	CHECK_UINT(count_send("PRACK"), 0);
	CHECK_UINT(ev.proceeding, 1);

	CASE("prack uac: a 100 with the option tag is ignored");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	rel_prov(100, 10, "peer-tag", NULL, NULL);
	CHECK_UINT(count_send("PRACK"), 0);
	CHECK_UINT(ev.proceeding, 0);

	CASE("prack uac: a reliable 1xx with no To tag is treated as plain");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	rel_prov(180, 10, NULL, NULL, NULL);
	CHECK_UINT(count_send("PRACK"), 0);
	CHECK_UINT(tr.prack_no_dialog_lines, 1);
	CHECK_UINT(ev.proceeding, 1);

	CASE("prack uac: Require without an RSeq is plain");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	prov_at_invite(180, "peer-tag", "Require: 100rel\r\n", NULL);
	CHECK_UINT(count_send("PRACK"), 0);
	CHECK_UINT(tr.prack_no_rseq_lines, 1);
	reply_at(0, 200, "peer-tag", NULL,
		 sdp_body(T_PEER, 20000, "0 101", NULL));
	CHECK_UINT(ev.answered, 1);

	CASE("prack uac: RSeq 0 is no sequence number, so the 1xx is plain");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	rel_prov(180, 0, "peer-tag", NULL, NULL);
	CHECK_UINT(count_send("PRACK"), 0);
	CHECK_UINT(tr.prack_no_rseq_lines, 1);

	CASE("prack uac: RSeq 4294967295 is reliable and the RAck names it");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	rel_prov(180, 4294967295u, "peer-tag", NULL, NULL);
	CHECK_UINT(count_send("PRACK"), 1);
	CHECK(has(prack_text(), "RAck: 4294967295 1 INVITE\r\n"));

	CASE("prack uac: a reliable 183 answer starts the early stream");
	fx_reset();
	cfg.channel[0].early_media_enabled = true;
	CHECK(call_dial(0, 0, "1002", true));
	rel_prov(183, 10, "peer-tag", NULL, sdp_body(T_PEER, 20000, "0", NULL));
	CHECK_UINT(count_send("PRACK"), 1);
	CHECK(ev.early_media);
	CHECK_UINT(med.last.direction, HAL_MEDIA_SENDRECV);
	CHECK_UINT(tr.prack_answer_lines, 1);

	CASE("prack uac: a 2xx with no body completes that call sendrecv");
	reply_at(0, 200, "peer-tag", NULL, NULL);
	CHECK_UINT(ev.answered, 1);
	CHECK_UINT(ev.failed, 0);
	CHECK_UINT(med.last.direction, HAL_MEDIA_SENDRECV);
	CHECK_UINT(med.last.remote.port, 20000);

	CASE("prack uac: a 2xx body behind a reliable answer is ignored");
	fx_reset();
	cfg.channel[0].early_media_enabled = true;
	CHECK(call_dial(0, 0, "1002", true));
	rel_prov(183, 10, "peer-tag", NULL, sdp_body(T_PEER, 20000, "0", NULL));
	reply_at(0, 200, "peer-tag", NULL, sdp_body(T_PEER, 21000, "0", NULL));
	CHECK_UINT(ev.answered, 1);
	CHECK_UINT(med.last.remote.port, 20000);
	CHECK_UINT(tr.prack_body_lines, 1);

	CASE("prack uac: a reliable copy of a taken answer closes it");
	fx_reset();
	cfg.channel[0].early_media_enabled = true;
	CHECK(call_dial(0, 0, "1002", true));
	prov_at_invite(183, "peer-tag", NULL,
		       sdp_body(T_PEER, 20000, "0", NULL));
	CHECK(ev.early_media);
	rel_prov(183, 10, "peer-tag", NULL, sdp_body(T_PEER, 20000, "0", NULL));
	CHECK_UINT(count_send("PRACK"), 1);
	CHECK_UINT(tr.prack_answer_lines, 1);
	reply_at(0, 200, "peer-tag", NULL, NULL);
	CHECK_UINT(ev.answered, 1);
	CHECK_UINT(ev.failed, 0);
	CHECK_UINT(med.last.direction, HAL_MEDIA_SENDRECV);
	CHECK_UINT(med.last.remote.port, 20000);
	cfg.channel[0].early_media_enabled = false;

	CASE("prack uac: a reliable answer is taken with early media off");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	rel_prov(183, 10, "peer-tag", NULL, sdp_body(T_PEER, 20000, "0", NULL));
	CHECK_UINT(count_send("PRACK"), 1);
	CHECK(!ev.early_media);
	CHECK_UINT(med.config_calls, 0);
	CHECK_UINT(tr.prack_answer_lines, 1);

	CASE("prack uac: and the bodiless 2xx behind it connects");
	reply_at(0, 200, "peer-tag", NULL, NULL);
	CHECK_UINT(ev.answered, 1);
	CHECK_UINT(ev.failed, 0);
	CHECK_UINT(med.last.direction, HAL_MEDIA_SENDRECV);
	CHECK_UINT(med.last.remote.port, 20000);

	CASE("prack uac: a reliable 1xx behind the 2xx draws nothing");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	rel_prov(180, 10, "peer-tag", NULL, NULL);
	reply_at(0, 200, "peer-tag", NULL,
		 sdp_body(T_PEER, 20000, "0 101", NULL));
	CHECK_UINT(ev.answered, 1);
	rel_prov(180, 11, "peer-tag", NULL, NULL);
	CHECK_UINT(count_send("PRACK"), 1);

	CASE("prack uac: a 481 to the PRACK leaves the INVITE running");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	rel_prov(180, 10, "peer-tag", NULL, NULL);
	reply(481, NULL, NULL, NULL);
	CHECK_UINT(tr.prack_refused_lines, 1);
	CHECK_UINT(ev.failed, 0);
	reply_at(0, 200, "peer-tag", NULL,
		 sdp_body(T_PEER, 20000, "0 101", NULL));
	CHECK_UINT(ev.answered, 1);

	CASE("prack uac: a PRACK that gets no answer leaves it running too");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	rel_prov(180, 10, "peer-tag", NULL, NULL);
	fake_advance(32000);
	CHECK_UINT(tr.prack_dead_lines, 1);
	CHECK_UINT(ev.failed, 0);
	reply_at(0, 200, "peer-tag", NULL,
		 sdp_body(T_PEER, 20000, "0 101", NULL));
	CHECK_UINT(ev.answered, 1);

	CASE("prack uac: a hangup with the PRACK in flight cancels at once");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	rel_prov(180, 10, "peer-tag", NULL, NULL);
	CHECK(call_hangup(0, 0));
	CHECK_UINT(count_send("CANCEL"), 1);
	reply_at((unsigned int)find_send("PRACK"), 200, NULL, NULL, NULL);
	CHECK_UINT(ev.failed, 0);
	CHECK_UINT(count_send("CANCEL"), 1);

	CASE("prack uac: the CANCEL behind a PRACK carries the INVITE branch");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	CHECK(req_info_at(0, &inv));
	rel_prov(180, 10, "peer-tag", NULL, NULL);
	CHECK(call_hangup(0, 0));
	idx = find_send("CANCEL");
	CHECK(idx > 0);
	CHECK(req_info_at((unsigned int)idx, &ri));
	CHECK(!strcmp(ri.branch, inv.branch));

	CASE("prack uac: the peer's 200 matches it, none is retransmitted");
	reply_at((unsigned int)idx, 200, NULL, NULL, NULL);
	fake_advance(32000);
	CHECK_UINT(count_send("CANCEL"), 1);

	CASE("prack uac: the 487 behind it is acknowledged and the slot frees");
	reply_at(0, 487, "peer-tag", NULL, NULL);
	CHECK(find_send("ACK") > 0);
	CHECK(slot_is(0));

	CASE("prack uac: a hangup before a provisional cancels on a 180");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	CHECK(call_hangup(0, 0));
	CHECK(find_send("CANCEL") < 0);
	rel_prov(180, 10, "peer-tag", NULL, NULL);
	CHECK_UINT(count_send("CANCEL"), 1);
	CHECK_UINT(count_send("PRACK"), 0);

	CASE("prack uac: the PRACK of a retried INVITE names the second CSeq");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	rel_prov(180, 10, "peer-tag", NULL, NULL);
	CHECK_UINT(count_send("PRACK"), 1);
	reply_at(0, 401, "peer-tag", chal, NULL);
	CHECK_UINT(count_send("INVITE"), 2);
	rel_prov(180, 10, "peer-tag", NULL, NULL);
	CHECK_UINT(count_send("PRACK"), 2);
	CHECK(has(prack_text(), "RAck: 10 2 INVITE\r\n"));
	CHECK(has(prack_text(), "Authorization: Digest"));
	CHECK(req_of("PRACK", &ri));
	CHECK_UINT(ri.cseq, 3);

	CASE("prack uac: a late challenge to the old PRACK is ignored");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	rel_prov(180, 10, "peer-tag", NULL, NULL);
	idx = find_send("PRACK");
	CHECK(idx > 0);
	reply_at(0, 401, "peer-tag", chal, NULL);
	CHECK_UINT(count_send("INVITE"), 2);
	reply_at((unsigned int)idx, 401, NULL, chal, NULL);
	CHECK_UINT(count_send("PRACK"), 1);

	CASE("prack uac: the 491 retry restarts the RSeq bookkeeping");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	rel_prov(180, 10, "peer-tag", NULL, NULL);
	CHECK_UINT(count_send("PRACK"), 1);
	reply_at(0, 491, "peer-tag", NULL, NULL);
	fake_advance(4000);
	CHECK(req_of("INVITE", &ri));
	CHECK_UINT(ri.cseq, 2);
	/* the wire carries the retransmissions of the wait too, so the count
	 * of accepted reliable 1xx is the oracle here */
	rel_prov(180, 10, "peer-tag", NULL, NULL);
	CHECK_UINT(tr.prack_sent_lines, 2);
	CHECK(has(prack_text(), "RAck: 10 2 INVITE\r\n"));

	CASE("prack uac: and so does the 422 retry");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	rel_prov(180, 10, "peer-tag", NULL, NULL);
	CHECK_UINT(count_send("PRACK"), 1);
	reply_at(0, 422, "peer-tag", "Min-SE: 900\r\n", NULL);
	CHECK_UINT(count_send("INVITE"), 2);
	rel_prov(180, 10, "peer-tag", NULL, NULL);
	CHECK_UINT(count_send("PRACK"), 2);
	CHECK(has(prack_text(), "RAck: 10 2 INVITE\r\n"));

	CASE("prack uac: timer C ends the setup with a PRACK outstanding");
	fx_reset();
	cfg.channel[0].timer_c_ms = 5000;
	CHECK(call_dial(0, 0, "1002", true));
	rel_prov(180, 10, "peer-tag", NULL, NULL);
	fake_advance(5000);
	CHECK_UINT(count_send("CANCEL"), 1);
	CHECK_UINT(ev.failed, 1);
	cfg.channel[0].timer_c_ms = 0;

	CASE("prack uac: both INVITEs of a challenged call offer 100rel");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	reply_at(0, 401, "peer-tag", chal, NULL);
	idx = find_send("INVITE");
	CHECK(idx > 0);
	CHECK(has(send_text(0), "Supported: timer, replaces, 100rel\r\n"));
	CHECK(has(send_text(0), "Allow: INVITE, ACK, CANCEL, BYE, OPTIONS, "
				"INFO, NOTIFY, REFER, PRACK\r\n"));
	CHECK(has(send_text((unsigned int)idx),
		  "Supported: timer, replaces, 100rel\r\n"));
	CHECK(has(send_text((unsigned int)idx),
		  "Allow: INVITE, ACK, CANCEL, BYE, OPTIONS, INFO, NOTIFY, "
		  "REFER, PRACK\r\n"));

	CASE("prack uac: our own INVITE never insists on 100rel");
	fx_reset();
	cfg.channel[0].prack = true;
	CHECK(call_dial(0, 0, "1002", true));
	CHECK(!has(send_text(0), "Require:"));
	cfg.channel[0].prack = false;
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	CHECK(!has(send_text(0), "Require:"));

	CASE("prack uac: a reliable 1xx to our own re-INVITE draws no PRACK");
	fx_reset();
	uac_up(NULL);
	CHECK(call_hold(0, 0));
	CHECK_UINT(count_send("INVITE"), 2);
	rel_prov(183, 10, "peer-tag", NULL, NULL);
	CHECK_UINT(count_send("PRACK"), 0);
	CHECK_UINT(tr.prack_sent_lines, 0);

	CASE("prack uac: a PRACK the transport refuses keeps the record");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	fk.send_fail = true;
	rel_prov(180, 10, "peer-tag", NULL, NULL);
	fk.send_fail = false;
	CHECK_UINT(count_send("PRACK"), 0);
	CHECK_UINT(tr.prack_unsent_lines, 1);

	CASE("prack uac: so the retransmitted 180 draws a second attempt");
	rel_prov(180, 10, "peer-tag", NULL, NULL);
	CHECK_UINT(count_send("PRACK"), 1);
	CHECK_UINT(tr.prack_retx_lines, 0);
	CHECK_UINT(tr.prack_sent_lines, 1);

	CASE("prack uac: a discarded 1xx does not move the dialog target");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	rel_prov(183, 10, "peer-tag", NULL, NULL);
	CHECK(req_of("PRACK", &ri));
	CHECK(!strcmp(ri.ruri, "sip:1002@" T_PEER ":5060"));
	rel_prov(180, 12, "peer-tag", contact_b, NULL);
	CHECK_UINT(count_send("PRACK"), 1);
	fk.no_contact = true;
	rel_prov(180, 11, "peer-tag", NULL, NULL);
	fk.no_contact = false;
	CHECK_UINT(count_send("PRACK"), 2);
	CHECK(req_of("PRACK", &ri));
	CHECK(!strcmp(ri.ruri, "sip:1002@" T_PEER ":5060"));

	CASE("prack uac: an in-order one does move it");
	rel_prov(180, 12, "peer-tag", contact_b, NULL);
	CHECK_UINT(count_send("PRACK"), 3);
	CHECK(req_of("PRACK", &ri));
	CHECK(!strcmp(ri.ruri, "sip:1002@" T_PEER ":5061"));

	/*
	 * RFC 3262 3 lets the 2xx of the INVITE overtake the answer to the
	 * PRACK. One credential retry serves every request of the dialog, and
	 * bye_send() clears that counter for the BYE, so a PRACK retry behind
	 * the BYE would leave the challenge of the BYE unanswered and the peer
	 * would keep the dialog.
	 */
	CASE("prack uac: a 401 to the PRACK behind the BYE draws no retry");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	rel_prov(180, 10, "peer-tag", NULL, NULL);
	idx = find_send("PRACK");
	CHECK(idx > 0);
	reply_at(0, 200, "peer-tag", NULL,
		 sdp_body(T_PEER, 20000, "0 101", NULL));
	CHECK_UINT(ev.answered, 1);
	CHECK(call_hangup(0, 0));
	CHECK_UINT(count_send("BYE"), 1);
	reply_at((unsigned int)idx, 401, NULL, chal, NULL);
	CHECK_UINT(count_send("PRACK"), 1);

	CASE("prack uac: and the BYE still answers its own challenge");
	idx = find_send("BYE");
	CHECK(idx > 0);
	reply_at((unsigned int)idx, 401, NULL, chal_b, NULL);
	CHECK_UINT(count_send("BYE"), 2);
	idx = find_send("BYE");
	CHECK(idx > 0);
	CHECK(has(send_text((unsigned int)idx), "Authorization: Digest"));

	/*
	 * RFC 3262 3 forbids a second reliable provisional only before the
	 * first one is acknowledged, so a conforming peer can send RSeq 12
	 * before our PRACK for 11 is answered. The answer to the older PRACK
	 * is recognised by its CSeq method and retried from its own request.
	 */
	CASE("prack uac: a 401 to the older of two PRACKs draws its retry");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	rel_prov(180, 11, "peer-tag", NULL, NULL);
	idx = find_send("PRACK");
	CHECK(idx > 0);
	rel_prov(180, 12, "peer-tag", NULL, NULL);
	CHECK_UINT(count_send("PRACK"), 2);
	reply_at((unsigned int)idx, 401, NULL, chal, NULL);
	CHECK_UINT(count_send("PRACK"), 3);
	CHECK(has(prack_text(), "RAck: 11 1 INVITE\r\n"));
	CHECK(has(prack_text(), "Authorization: Digest"));

	/* the RAck of a PRACK of the exchange that the retry ended names a
	 * CSeq the INVITE in flight no longer carries */
	CASE("prack uac: a fresh challenge to a superseded PRACK draws none");
	fx_reset();
	CHECK(call_dial(0, 0, "1002", true));
	rel_prov(180, 10, "peer-tag", NULL, NULL);
	idx = find_send("PRACK");
	CHECK(idx > 0);
	reply_at(0, 401, "peer-tag", chal, NULL);
	CHECK_UINT(count_send("INVITE"), 2);
	reply_at((unsigned int)idx, 401, NULL, chal_b, NULL);
	CHECK_UINT(count_send("PRACK"), 1);
}

/* the RSeq of one response this side sent, 0 when it carries none */
static uint32_t resp_rseq_at(int idx)
{
	static char copy[WIRE_MAX];
	struct sip_msg m;
	uint32_t rseq = 0;
	size_t len;

	if (idx < 0 || (unsigned int)idx >= fk.sends || idx >= FAKE_SEND_MAX)
		return 0;

	len = fk.send[idx].len;
	memcpy(copy, fk.send[idx].buf, len);
	if (msg_parse(&m, copy, len) != SIP_PARSE_OK || m.is_request)
		return 0;

	return hdr_rseq_get(&m, &rseq) ? rseq : 0;
}

/*
 * A PRACK from the peer inside the early dialog our own reliable 180 created.
 * The tags come off that 180 and not off a 2xx, because a PRACK arrives while
 * the call is still ringing. icseq is the CSeq of the INVITE the RAck names
 * (RFC 3262 7.2), and a NULL method leaves the RAck off altogether.
 */
static const char *prack_req(unsigned int cseq, uint32_t rseq, uint32_t icseq,
			     const char *method, const char *body)
{
	char call_id[160], peer_tag[64], our_tag[64], rack[64];

	if (!resp_tags_at(find_send("SIP/2.0 180"), call_id, sizeof(call_id),
			  peer_tag, sizeof(peer_tag), our_tag, sizeof(our_tag)))
		return "";

	rack[0] = 0;
	if (method)
		snprintf(rack, sizeof(rack), "RAck: %u %u %s\r\n", rseq,
			 icseq, method);

	return dialog_req_text("PRACK", cseq, call_id, peer_tag, our_tag,
			       rack, body);
}

/*
 * The ACK of a non-2xx final this side sent (RFC 3261 17.1.1.3). It carries
 * the Via branch of the inbound INVITE, so the server transaction absorbs it,
 * and tag is the To tag of that final response.
 */
static const char *reject_ack_text(const char *call_id, const char *tag)
{
	char *buf = text_slot();

	snprintf(buf, TEXT_MAX,
		 "ACK sip:%s@%s SIP/2.0\r\n"
		 "Via: SIP/2.0/UDP %s:%u;branch=z9hG4bK-in-%s\r\n"
		 "Max-Forwards: 70\r\n"
		 "From: \"Alice\" <sip:1002@%s>;tag=peer-%s\r\n"
		 "To: <sip:%s@%s>;tag=%s\r\n"
		 "Call-ID: %s\r\n"
		 "CSeq: 1 ACK\r\n"
		 "Content-Length: 0\r\n"
		 "\r\n",
		 T_USER, T_DOMAIN, T_PEER, T_PEER_PORT, call_id, T_DOMAIN,
		 call_id, T_USER, T_DOMAIN, tag, call_id);

	return buf;
}

/* an inbound INVITE that insists on a reliable provisional response */
static const char *rel_invite(const char *call_id)
{
	return invite_text(call_id, "Require: 100rel\r\n",
			   sdp_body(T_PEER, 20000, "0", NULL));
}

/*
 * The shape a switch sends: one Supported header that names both option tags.
 * The fixture's own builder writes "Supported: timer" and splices the extra
 * block behind it, which yields two headers instead.
 */
static const char *sup_invite(const char *call_id)
{
	const char *body = sdp_body(T_PEER, 20000, "0", NULL);
	char *buf = text_slot();

	snprintf(buf, TEXT_MAX,
		 "INVITE sip:%s@%s SIP/2.0\r\n"
		 "Via: SIP/2.0/UDP %s:%u;branch=z9hG4bK-in-%s\r\n"
		 "Max-Forwards: 70\r\n"
		 "From: \"Alice\" <sip:1002@%s>;tag=peer-%s\r\n"
		 "To: <sip:%s@%s>\r\n"
		 "Call-ID: %s\r\n"
		 "CSeq: 1 INVITE\r\n"
		 "Contact: <sip:1002@%s:%u>\r\n"
		 "Supported: timer, 100rel\r\n"
		 "Content-Type: application/sdp\r\n"
		 "Content-Length: %u\r\n"
		 "\r\n"
		 "%s",
		 T_USER, T_DOMAIN, T_PEER, T_PEER_PORT, call_id, T_DOMAIN,
		 call_id, T_USER, T_DOMAIN, call_id, T_PEER, T_PEER_PORT,
		 (unsigned int)strlen(body), body);

	return buf;
}

/*
 * In the UAS half of RFC 3262, the 180 goes out reliably under either tag
 * when the prack option is set. It repeats from T1 with no cap until its
 * PRACK arrives, and the INVITE is rejected 504 at 64*T1. The option is set
 * for the whole function, because it gates every reliable 180 here. A case
 * that wants the plain answer clears the option and sets it again.
 *
 * Each PRACK carries a CSeq one above the last, because the dialog layer
 * answers a repeated or lower number 500 before rx_prack() runs. Each number
 * also draws its own Via branch, so a repeat would be absorbed as a
 * retransmission.
 */
static void test_prack_uas(void)
{
	char tag180[64], tag487[64];
	uint32_t rseq, delay;
	unsigned int sends;

	cfg.channel[0].prack = true;

	CASE("prack uas: Require: 100rel is reliable with the option set");
	fx_reset();
	rx(rel_invite("rel-1"));
	CHECK(call_ring(0, 0));
	CHECK(has(send_text(0), "SIP/2.0 180"));
	CHECK(has(send_text(0), "Require: 100rel\r\n"));
	rseq = resp_rseq_at(0);
	CHECK(rseq >= 1 && rseq <= 2147483647u);
	CHECK_UINT(tr.rel_sent_lines, 1);

	CASE("prack uas: the schedule starts at T1");
	CHECK_UINT(timer_delay(SIP_TIMER_CALL_REL), 500);

	CASE("prack uas: no automatic 100 follows our own reliable 180");
	fake_advance(300);
	CHECK_UINT(fk.sends, 1);

	/* the cadence doubles without the T2 cap of a 2xx (RFC 3262 3) */
	CASE("prack uas: the 180 goes out again at 500, 1500 and 3500 ms");
	fake_advance(200);
	CHECK_UINT(count_send("SIP/2.0 180"), 2);
	fake_advance(1000);
	CHECK_UINT(count_send("SIP/2.0 180"), 3);
	fake_advance(2000);
	CHECK_UINT(count_send("SIP/2.0 180"), 4);
	CHECK_UINT(tr.rel_retx_lines, 3);

	CASE("prack uas: every copy carries the RSeq of the first");
	CHECK_UINT(resp_rseq_at(1), rseq);
	CHECK_UINT(resp_rseq_at(3), rseq);

	CASE("prack uas: the matching PRACK earns 200 and stops the schedule");
	rx(prack_req(2, rseq, 1, "INVITE", NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(!timer_armed(SIP_TIMER_CALL_REL));
	CHECK_UINT(tr.rel_match_lines, 1);
	sends = count_send("SIP/2.0 180");
	fake_advance(40000);
	CHECK_UINT(count_send("SIP/2.0 180"), sends);

	CASE("prack uas: a PRACK that names another RSeq is answered 481");
	fx_reset();
	rx(rel_invite("rel-2"));
	CHECK(call_ring(0, 0));
	rseq = resp_rseq_at(0);
	rx(prack_req(2, rseq + 1, 1, "INVITE", NULL));
	CHECK(has(last_text(), "SIP/2.0 481"));

	CASE("prack uas: one that names another CSeq is answered 481");
	rx(prack_req(3, rseq, 2, "INVITE", NULL));
	CHECK(has(last_text(), "SIP/2.0 481"));

	CASE("prack uas: one whose RAck method is not INVITE is answered 481");
	rx(prack_req(4, rseq, 1, "BYE", NULL));
	CHECK(has(last_text(), "SIP/2.0 481"));

	CASE("prack uas: one without a RAck at all is answered 481");
	rx(prack_req(5, rseq, 1, NULL, NULL));
	CHECK(has(last_text(), "SIP/2.0 481"));
	CHECK_UINT(tr.rel_nomatch_lines, 4);

	CASE("prack uas: the record stands and the right PRACK earns 200");
	CHECK(timer_armed(SIP_TIMER_CALL_REL));
	rx(prack_req(6, rseq, 1, "INVITE", NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(tr.rel_match_lines, 1);

	CASE("prack uas: a stale CSeq is answered 500 (RFC 3261 12.2.2)");
	rx(prack_req(1, rseq, 1, "INVITE", NULL));
	CHECK(has(last_text(), "SIP/2.0 500"));

	CASE("prack uas: a PRACK for no dialog at all is answered 481");
	fx_reset();
	rx(dialog_req_text("PRACK", 2, "no-such-dialog", "peer", "us",
			   "RAck: 1 1 INVITE\r\n", NULL));
	CHECK(has(last_text(), "SIP/2.0 481"));

	CASE("prack uas: 64*T1 without a PRACK rejects the INVITE 504");
	fx_reset();
	rx(rel_invite("rel-3"));
	CHECK(call_ring(0, 0));
	fake_advance(31999);
	CHECK_UINT(count_send("SIP/2.0 180"), 7);
	CHECK(find_send("SIP/2.0 504") < 0);
	fake_advance(1);
	CHECK(has(last_text(), "SIP/2.0 504"));
	CHECK_UINT(count_send("SIP/2.0 180"), 7);
	CHECK_UINT(tr.rel_giveup_lines, 1);

	CASE("prack uas: the leg fails with congestion and frees its slot");
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.cls, CALL_FAIL_CONGESTION);
	CHECK(slot_is(0));
	CHECK(!timer_armed(SIP_TIMER_CALL_REL));

	/* the last arm runs to the deadline, and the fire that follows it is
	 * the give-up whatever the clock reads then */
	CASE("prack uas: the give-up stands when that timer fires early");
	fx_reset();
	rx(rel_invite("rel-3b"));
	CHECK(call_ring(0, 0));
	fake_advance(31500);
	CHECK_UINT(count_send("SIP/2.0 180"), 7);
	CHECK_UINT(timer_delay(SIP_TIMER_CALL_REL), 500);
	timer_fire_early(SIP_TIMER_CALL_REL, 1);
	CHECK(has(last_text(), "SIP/2.0 504"));
	CHECK_UINT(count_send("SIP/2.0 180"), 7);
	CHECK_UINT(tr.rel_giveup_lines, 1);
	CHECK_UINT(ev.failed, 1);
	CHECK(!timer_armed(SIP_TIMER_CALL_REL));

	CASE("prack uas: a retransmission the transport refuses ends the leg");
	fx_reset();
	rx(rel_invite("rel-4"));
	CHECK(call_ring(0, 0));
	fk.send_fail = true;
	fake_advance(500);
	fk.send_fail = false;
	/* the leg fails once, in call_txn_done(), and the timer callback that
	 * asked for the copy touches it no more */
	CHECK_UINT(tr.ist_fail_lines, 1);
	CHECK_UINT(ev.failed, 1);
	CHECK_UINT(ev.cls, CALL_FAIL_CONGESTION);
	CHECK(slot_is(0));
	CHECK(!timer_armed(SIP_TIMER_CALL_REL));

	CASE("prack uas: a CANCEL under an unacknowledged 180 ends the leg");
	fx_reset();
	rx(rel_invite("rel-5"));
	CHECK(call_ring(0, 0));
	CHECK(resp_to_tag(0, tag180, sizeof(tag180)));
	rx(cancel_text("rel-5"));
	CHECK(has(send_text(1), "SIP/2.0 200"));
	CHECK(has(send_text(2), "SIP/2.0 487"));

	CASE("prack uas: the 487 carries the tag of the reliable 180");
	CHECK(resp_to_tag(2, tag487, sizeof(tag487)));
	CHECK(!strcmp(tag180, tag487));
	fake_advance(40000);
	CHECK_UINT(count_send("SIP/2.0 180"), 1);
	CHECK(slot_is(0));
	CHECK(!timer_armed(SIP_TIMER_CALL_REL));

	/* the PRACK crosses our own rejection: the leg and its dialog are
	 * gone, so the request never reaches the match rule of RFC 3262 3 */
	CASE("prack uas: a PRACK that crosses our own 487 is answered 481");
	fx_reset();
	rx(rel_invite("rel-5b"));
	CHECK(call_ring(0, 0));
	rseq = resp_rseq_at(0);
	rx(cancel_text("rel-5b"));
	CHECK(has(send_text(2), "SIP/2.0 487"));
	CHECK(resp_to_tag(2, tag487, sizeof(tag487)));
	rx(reject_ack_text("rel-5b", tag487));
	rx(prack_req(2, rseq, 1, "INVITE", NULL));
	CHECK(has(last_text(), "SIP/2.0 481"));

	CASE("prack uas: that 481 is the dialog answer, not the PRACK one");
	CHECK_UINT(tr.rel_nomatch_lines, 0);
	CHECK(slot_is(0));

	CASE("prack uas: the 2xx may precede the PRACK and stops the 180");
	fx_reset();
	rx(rel_invite("rel-6"));
	CHECK(call_ring(0, 0));
	rseq = resp_rseq_at(0);
	CHECK(call_answer(0, 0));
	CHECK(has(send_text(1), "SIP/2.0 200"));
	CHECK(!timer_armed(SIP_TIMER_CALL_REL));
	fake_advance(2000);
	CHECK_UINT(count_send("SIP/2.0 180"), 1);

	CASE("prack uas: a PRACK behind the 2xx still earns 200");
	rx(prack_req(2, rseq, 1, "INVITE", NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK_UINT(tr.rel_match_lines, 1);

	CASE("prack uas: and the ACK completes the exchange");
	rx(uas_in_dialog("ACK", 1, NULL, NULL));
	CHECK_UINT(med.start_calls, 1);
	CHECK_UINT(ev.failed, 0);

	CASE("prack uas: a retransmitted INVITE replays the same 180");
	fx_reset();
	rx(rel_invite("rel-7"));
	CHECK(call_ring(0, 0));
	rseq = resp_rseq_at(0);
	delay = (uint32_t)timer_delay(SIP_TIMER_CALL_REL);
	rx(rel_invite("rel-7"));
	CHECK_UINT(count_send("SIP/2.0 180"), 2);
	CHECK_UINT(resp_rseq_at(1), rseq);
	CHECK_UINT(timer_delay(SIP_TIMER_CALL_REL), delay);
	CHECK_UINT(tr.rel_retx_lines, 0);

	CASE("prack uas: a second ring sends no second reliable 180");
	CHECK(call_ring(0, 0));
	CHECK_UINT(count_send("SIP/2.0 180"), 2);
	CHECK_UINT(tr.rel_sent_lines, 1);

	CASE("prack uas: Supported: 100rel is reliable with the option set");
	fx_reset();
	rx(invite_text("rel-8", "Supported: 100rel\r\n",
		       sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(call_ring(0, 0));
	CHECK(has(send_text(0), "Require: 100rel\r\n"));
	CHECK(resp_rseq_at(0) != 0);
	CHECK(timer_armed(SIP_TIMER_CALL_REL));

	CASE("prack uas: the option tag is read whatever its case");
	fx_reset();
	rx(invite_text("rel-9", "Supported: 100REL\r\n",
		       sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(call_ring(0, 0));
	CHECK(has(send_text(0), "Require: 100rel\r\n"));

	CASE("prack uas: one Supported header naming both tags is read too");
	fx_reset();
	rx(sup_invite("rel-19"));
	CHECK(call_ring(0, 0));
	CHECK(has(send_text(0), "Require: 100rel\r\n"));
	CHECK(resp_rseq_at(0) != 0);

	CASE("prack uas: neither tag leaves the 180 plain with the option");
	fx_reset();
	rx(invite_text("rel-10", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(call_ring(0, 0));
	CHECK(!has(send_text(0), "Require: 100rel"));
	CHECK(!has(send_text(0), "RSeq:"));
	CHECK(!timer_armed(SIP_TIMER_CALL_REL));

	CASE("prack uas: Supported: 100rel with the option clear stays plain");
	fx_reset();
	cfg.channel[0].prack = false;
	rx(invite_text("rel-11", "Supported: 100rel\r\n",
		       sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(call_ring(0, 0));
	CHECK(!has(send_text(0), "Require: 100rel"));
	CHECK(!has(send_text(0), "RSeq:"));
	CHECK(!timer_armed(SIP_TIMER_CALL_REL));
	fake_advance(2000);
	CHECK_UINT(count_send("SIP/2.0 180"), 1);
	CHECK_UINT(tr.rel_sent_lines, 0);
	cfg.channel[0].prack = true;

	/*
	 * The deviation from the MUST of RFC 3262 section 3.
	 */
	CASE("prack uas: Require: 100rel with the option clear stays plain");
	fx_reset();
	cfg.channel[0].prack = false;
	rx(rel_invite("rel-11b"));
	CHECK(call_ring(0, 0));
	CHECK(!has(send_text(0), "Require: 100rel"));
	CHECK(!has(send_text(0), "RSeq:"));
	CHECK(!timer_armed(SIP_TIMER_CALL_REL));
	fake_advance(2000);
	CHECK_UINT(count_send("SIP/2.0 180"), 1);
	CHECK_UINT(tr.rel_sent_lines, 0);

	CASE("prack uas: a PRACK behind that plain 180 is answered 481");
	rx(prack_req(2, 1, 1, "INVITE", NULL));
	CHECK(has(last_text(), "SIP/2.0 481"));
	CHECK_UINT(tr.rel_nomatch_lines, 1);
	CHECK(call_answer(0, 0));
	CHECK(has(last_text(), "SIP/2.0 200"));
	cfg.channel[0].prack = true;

	CASE("prack uas: the 180 retransmits over a reliable transport too");
	fx_reset();
	fk.env.flags = SIP_TXN_ENV_RELIABLE;
	rx(rel_invite("rel-12"));
	CHECK(call_ring(0, 0));
	fake_advance(1500);
	CHECK_UINT(count_send("SIP/2.0 180"), 3);
	fk.env.flags = 0;

	CASE("prack uas: a released leg leaves no reliable timer behind");
	fx_reset();
	rx(rel_invite("rel-13"));
	CHECK(call_ring(0, 0));
	CHECK(call_reject(0, 0, 486));
	CHECK(slot_is(0));
	CHECK(!timer_armed(SIP_TIMER_CALL_REL));
	rx(invite_text("rel-14", NULL, sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(call_ring(0, 0));
	CHECK(!timer_armed(SIP_TIMER_CALL_REL));
	fake_advance(40000);
	CHECK_UINT(ev.failed, 0);

	CASE("prack uas: a reload keeps the snapshot the leg started with");
	fx_reset();
	rx(invite_text("rel-15", "Supported: 100rel\r\n",
		       sdp_body(T_PEER, 20000, "0", NULL)));
	cfg.channel[0].prack = false;
	calls_reload(&cfg);
	CHECK(call_ring(0, 0));
	CHECK(has(send_text(0), "Require: 100rel\r\n"));
	cfg.channel[0].prack = true;

	CASE("prack uas: a body on the PRACK is ignored, its 200 has none");
	fx_reset();
	rx(rel_invite("rel-16"));
	CHECK(call_ring(0, 0));
	rseq = resp_rseq_at(0);
	rx(prack_req(2, rseq, 1, "INVITE",
		     sdp_body(T_PEER, 21000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(has(last_text(), "Content-Length: 0\r\n"));
	CHECK(!has(last_text(), "m=audio"));
	CHECK_UINT(med.config_calls, 0);

	CASE("prack uas: Require: 100rel, foo is still refused 420");
	fx_reset();
	rx(invite_text("rel-17", "Require: 100rel, foo\r\n",
		       sdp_body(T_PEER, 20000, "0", NULL)));
	CHECK(has(last_text(), "SIP/2.0 420"));
	CHECK(has(last_text(), "Unsupported: foo\r\n"));
	CHECK_UINT(ev.incoming, 0);

	/*
	 * RFC 3262 5 asks for the offer in the first reliable provisional of an
	 * INVITE that carries none. uvoice keeps it in the 2xx instead, which
	 * needs no answer out of the PRACK (call.c rel_wanted).
	 */
	CASE("prack uas: an offerless INVITE takes a bodiless reliable 180");
	fx_reset();
	rx(invite_text("rel-18", "Require: 100rel\r\n", NULL));
	CHECK(call_ring(0, 0));
	CHECK(has(send_text(0), "Require: 100rel\r\n"));
	CHECK(has(send_text(0), "Content-Length: 0\r\n"));
	CHECK(!has(send_text(0), "m=audio"));
	rseq = resp_rseq_at(0);

	CASE("prack uas: its offer stays in the 2xx and the ACK answers it");
	rx(prack_req(2, rseq, 1, "INVITE", NULL));
	CHECK(has(last_text(), "SIP/2.0 200"));
	CHECK(call_answer(0, 0));
	CHECK(has(last_text(), "m=audio 10000 RTP/AVP 0 8 18 101"));
	rx(uas_in_dialog("ACK", 1, NULL, sdp_body(T_PEER, 20000, "8", NULL)));
	CHECK_UINT(med.config_calls, 1);
	CHECK_UINT(med.last.codec, HAL_CODEC_PCMA);

	cfg.channel[0].prack = false;
}

int main(void)
{
	cfg_default();
	fx_reset();

	test_ports();
	test_port_range();
	test_sdp_local();
	test_media_fill();
	test_codec_mask();
	test_fail_map();
	test_uac();
	test_dial_escape();
	test_uac_auth();
	test_cancel();
	test_glare();
	test_glare_wait();
	test_min_se();
	test_session();
	test_session_refresher();
	test_session_answer_floor();
	test_session_min_se_keep();
	test_reinvite_supported();
	test_abandon_one_bye();
	test_timer_c();
	test_uas();
	test_ring_refuse();
	test_uas_offerless();
	test_inbound_cancel();
	test_inbound_bye();
	test_server_txn();
	test_reinvite_in();
	test_reinvite_out();
	test_legacy_hold();
	test_reinvite_out_uas();
	test_admission();
	test_refused();
	test_cid();
	test_cid_refused();
	test_alert_info();
	test_misc();
	test_cw_parse();
	test_cw_wire();
	test_dtmf_parse();
	test_dtmf_wire();
	test_forward();
	test_flash_relay();
	test_xfer_refusals();
	test_xfer_blind();
	test_xfer_attended();
	test_xfer_auth();
	test_xfer_auth_consult();
	test_xfer_overflow();
	test_xfer_void();
	test_xfer_consult();
	test_xfer_onhook();
	test_xfer_in_refusals();
	test_xfer_in_accept();
	test_xfer_in_child();
	test_service_route();
	test_xfer_in_swap();
	test_xfer_in_silence();
	test_xfer_in_failure();
	test_xfer_lifetime();
	test_xfer_two_names();
	test_xfer_allow_lists();
	test_replaces_admission();
	test_replaces_match();
	test_replaces_takeover();
	test_replaces_race();
	test_replaces_supported();
	test_fax();
	test_fax_auth();
	test_g729_annexb();
	test_fax_follow();
	test_fax_remote_kind();
	test_fax_exit();
	test_line_fax_kind();
	test_t38();
	test_srtp();
	test_srtp_wipe();
	test_media_addr();
	test_ipv6();
	test_media_device();
	test_ended();
	test_cleared();
	test_bye_sent_reinvite();
	test_shutdown();
	test_transport();
	test_prack_uac();
	test_prack_uas();

	test_rework();

	CASE("fixture: no timer was dropped");
	CHECK_UINT(fake_timer_dropped, 0);

	return tu_summary("test_call");
}
