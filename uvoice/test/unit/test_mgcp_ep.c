// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Unit tests for the MGCP endpoints and package tables
 * (src/proto/mgcp/ep.c, src/proto/mgcp/sig.c).
 *
 * The endpoints run over real sources: transaction layer, SDP layer, media
 * fill, dial plan and port pool. The suite moves a virtual clock by hand, and
 * a capture array replaces a socket. A recorder host table
 * (proto_host_fake.c) stands in for the core. A case injects call agent
 * datagrams and line events, then reads what went out and the host calls.
 */
#include <stdarg.h>
#include <stdlib.h>

#include "net/ports.h"
#include "proto/mgcp/ep.h"
#include "proto_host_fake.h"
#include "str.h"
#include "test_util.h"

#define FAKE_TIMER_MAX		256
#define FAKE_SEND_MAX		256
#define FAKE_WIRE_MAX		4200

struct fake_send {
	char buf[FAKE_WIRE_MAX + 1];
	size_t len;
	char host[64];
	uint16_t port;
	uint64_t at_ms;
};

static struct {
	struct mgcp_txn_env env;
	uint64_t now;

	struct proto_timer *timer[FAKE_TIMER_MAX];
	unsigned int timers;
	unsigned int fired;

	struct fake_send send[FAKE_SEND_MAX];
	unsigned int sends;
	bool send_fail;

	char entity[MGCP_ENTITY_MAX];
	bool dst_fail;
	uint32_t ca_txid;
} fk;

#include "test_mgcp_clock.h"

static struct mgcp_txn_mgr mgr;
static struct mgcp_gw gw;
static struct cfg cfg;
static struct hal_region region;
static struct sockaddr_storage ca_sa;
static socklen_t ca_len;
static unsigned int mark;
static unsigned int hop_mark;
static enum cfg_mgcp_dialect dialect;

#define EP1	"aaln/1@gw.example.net"
#define EP2	"aaln/2@gw.example.net"

/* ------------------------------------------------------------- fixture */

static bool fake_dst(void *priv, const char *entity,
		     struct sockaddr_storage *out, socklen_t *len)
{
	str_copy(fk.entity, sizeof(fk.entity), entity);
	if (fk.dst_fail)
		return false;
	*len = sa_v4(out, strstr(entity, "ca2") ? "192.0.2.2" : "192.0.2.1",
		     2727);

	return true;
}

static void up_command(struct mgcp_txn_env *env, const struct mgcp_msg *msg,
		       const struct sockaddr *src, socklen_t src_len)
{
	mgcp_ep_command(&gw, msg, src, src_len);
}

static void up_response(struct mgcp_txn_env *env, uint32_t owner,
			uint32_t txid, const struct mgcp_msg *msg)
{
	mgcp_ep_response(&gw, owner, txid, msg);
}

static void up_done(struct mgcp_txn_env *env, uint32_t owner, uint32_t txid,
		    enum mgcp_txn_reason reason)
{
	mgcp_ep_txn_done(&gw, owner, txid, reason);
}

static void up_disconnected(struct mgcp_txn_env *env)
{
	mgcp_ep_disconnected(&gw);
}

static void up_disc_attempt(struct mgcp_txn_env *env)
{
	mgcp_ep_disc_attempt(&gw);
}

static void up_disc_ended(struct mgcp_txn_env *env)
{
	mgcp_ep_disc_ended(&gw);
}

static uint32_t up_rand(struct proto_timer_env *env)
{
	return 0;
}

static void cfg_fill(void)
{
	unsigned int i;

	memset(&cfg, 0, sizeof(cfg));
	cfg.loaded = true;
	cfg.enabled = true;
	cfg.channels = 2;
	cfg.rtcp_enabled = false;

	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		struct cfg_channel *ch = &cfg.channel[i];

		ch->present = true;
		ch->enabled = true;
		ch->id = i;
		ch->ptime_ms = 20;
		ch->dtmf_mode = CFG_DTMF_RFC2833;
		ch->rtp_port_min = 16384;
		ch->rtp_port_max = 16483;
		ch->fax_t38_enable = true;
		ch->g711a_ptype = 8;
		ch->g729a_ptype = 18;
		ch->cid_remote_enable = true;
		ch->lec_enable = true;
		ch->interdigit_s = 4;
		ch->interdigit_long_s = 16;
		str_copy(ch->digit_map, sizeof(ch->digit_map), "x.T");
	}
}

static void region_fill(void)
{
	memset(&region, 0, sizeof(region));
	region.ring[0].cadence[0].on_ms = 2000;
	region.ring[0].cadence[0].off_ms = 4000;
	region.ring[0].cadence_count = 1;
	region.cw_count = 4;
	region.confirm_total_ms = 600;
}

struct fx {
	enum cfg_mgcp_dialect dialect;
	bool loop;
	const char *persistent;
};

static void fixture_start(const struct fx *f)
{
	static const struct mgcp_ep_env env = { .dst = fake_dst };
	struct mgcp_txn_cfg tc;
	struct mgcp_ep_cfg ec;

	memset(&fk, 0, sizeof(fk));
	proto_host_fake_reset();
	net_port_reset();
	cfg_fill();
	region_fill();

	fk.env.send = fake_send_fn;
	fk.env.command = up_command;
	fk.env.response = up_response;
	fk.env.done = up_done;
	fk.env.disconnected = up_disconnected;
	fk.env.disc_attempt = up_disc_attempt;
	fk.env.disc_ended = up_disc_ended;
	fake_clock_bind(&fk.env.base);
	fk.env.base.rand32 = up_rand;

	phf.cfg = &cfg;
	phf.region = &region;
	phf.hal_codecs = (1u << HAL_CODEC_PCMU) | (1u << HAL_CODEC_PCMA) |
			 (1u << HAL_CODEC_G729);
	phf.hal_features = HAL_FEAT_POLARITY | HAL_FEAT_T38;

	dialect = f->dialect;
	mgcp_txn_cfg_default(&tc);
	if (dialect == CFG_MGCP_NCS)
		tc.flags = MGCP_TXN_CFG_MAX2_QUIET | MGCP_TXN_CFG_TD_RANDOM;
	CHECK(mgcp_txn_mgr_init(&mgr, &fk.env, &tc) == 0);

	memset(&ec, 0, sizeof(ec));
	ec.dialect = dialect;
	ec.lines = 2;
	str_copy(ec.pattern, sizeof(ec.pattern), "aaln/{line}@{host}");
	ec.line_base = 1;
	str_copy(ec.host, sizeof(ec.host), "gw.example.net");
	str_copy(ec.entity, sizeof(ec.entity), "ca@192.0.2.1:2727");
	str_copy(ec.persistent, sizeof(ec.persistent),
		 f->persistent ? f->persistent : "hd,hu,hf");
	ec.quarantine_loop = f->loop;
	ec.restart_delay_max_ms = 10000;
	str_copy(ec.media_addr, sizeof(ec.media_addr), "192.0.2.10");
	CHECK(mgcp_ep_init(&gw, proto_host_fake(), &mgr, &env, &ec) == 0);

	ca_len = sa_v4(&ca_sa, "192.0.2.1", 2727);
	fk.ca_txid = 5000;
	mark = 0;
	hop_mark = 0;
}

static void fixture_stop(void)
{
	mgcp_ep_done(&gw);
	mgcp_txn_mgr_done(&mgr);
}

static void mark_set(void)
{
	mark = fk.sends;
	hop_mark = phf.nhop;
}

/* the first message sent since the mark that starts with prefix, or "" */
static const char *sent(const char *prefix)
{
	unsigned int i;

	for (i = mark; i < fk.sends && i < FAKE_SEND_SLOTS; i++) {
		if (!strncmp(fk.send[i].buf, prefix, strlen(prefix)))
			return fk.send[i].buf;
	}

	return "";
}

static unsigned int sent_count(const char *prefix)
{
	unsigned int i, n = 0;

	for (i = mark; i < fk.sends && i < FAKE_SEND_SLOTS; i++)
		n += !strncmp(fk.send[i].buf, prefix, strlen(prefix));

	return n;
}

static const struct fake_send *sent_rec(const char *prefix)
{
	unsigned int i;

	for (i = mark; i < fk.sends && i < FAKE_SEND_SLOTS; i++) {
		if (!strncmp(fk.send[i].buf, prefix, strlen(prefix)))
			return &fk.send[i];
	}

	return NULL;
}

/* the last message sent since the mark that starts with prefix and went to
 * host (any host for NULL), or NULL: the transaction still out, when an
 * attempt replaced the earlier ones */
static const struct fake_send *sent_to(const char *prefix, const char *host)
{
	unsigned int i;

	for (i = fk.sends < FAKE_SEND_SLOTS ? fk.sends : FAKE_SEND_SLOTS;
	     i > mark; i--) {
		const struct fake_send *s = &fk.send[i - 1];

		if (!strncmp(s->buf, prefix, strlen(prefix)) &&
		    (!host || !strcmp(s->host, host)))
			return s;
	}

	return NULL;
}

static uint32_t txid_of(const char *text)
{
	const char *sp = strchr(text, ' ');

	return sp ? (uint32_t)strtoul(sp + 1, NULL, 10) : 0;
}

static bool has(const char *text, const char *needle)
{
	return strstr(text, needle) != NULL;
}

static const char *version_text(void)
{
	return dialect == CFG_MGCP_NCS ? MGCP_VERSION_NCS :
					 MGCP_VERSION_RFC3435;
}

/*
 * One command from the call agent. rest holds the parameter lines and an
 * optional body, with CRLF line ends. The answer is the response text the
 * gateway sent for it, the RSIP of a piggyback included, or "".
 */
static const char *ca_cmd(const char *verb, const char *ep, const char *rest)
{
	char buf[FAKE_WIRE_MAX];
	uint32_t txid = ++fk.ca_txid;
	char want[32];
	unsigned int before = fk.sends, i;
	int n;

	n = snprintf(buf, sizeof(buf), "%s %u %s %s\r\n%s", verb, txid, ep,
		     version_text(), rest ? rest : "");
	mgcp_txn_recv(&mgr, buf, (size_t)n, (struct sockaddr *)&ca_sa,
		      ca_len);

	snprintf(want, sizeof(want), " %u ", txid);
	for (i = before; i < fk.sends && i < FAKE_SEND_SLOTS; i++) {
		const char *t = fk.send[i].buf;

		if (!strncmp(t, "RSIP", 4) && has(t, "\r\n.\r\n"))
			return t;
		if (strlen(t) > 4 && !strncmp(t + 3, want, strlen(want)))
			return t;
	}

	return "";
}

static unsigned int code_of(const char *resp)
{
	const char *line = resp;

	if (!strncmp(resp, "RSIP", 4)) {
		line = strstr(resp, "\r\n.\r\n");
		if (!line)
			return 0;
		line += 5;
	}

	return (unsigned int)strtoul(line, NULL, 10);
}

/* The call agent answers a command the gateway sent. */
static void ca_answer(const char *gw_text, unsigned int code, const char *rest)
{
	char buf[1024];
	int n;

	n = snprintf(buf, sizeof(buf), "%03u %u OK\r\n%s", code,
		     txid_of(gw_text), rest ? rest : "");
	mgcp_txn_recv(&mgr, buf, (size_t)n, (struct sockaddr *)&ca_sa,
		      ca_len);
}

static void ev_hook(unsigned int line, uint8_t state)
{
	struct proto_line_event ev = { .type = PROTO_EV_HOOK };

	ev.u.hook.state = state;
	mgcp_ep_line_event(&gw, line, &ev);
}

static void ev_digit(unsigned int line, char digit)
{
	struct proto_line_event ev = { .type = PROTO_EV_DIGIT };

	ev.u.digit.digit = digit;
	ev.u.digit.duration_ms = 80;
	ev.u.digit.measured = true;
	mgcp_ep_line_event(&gw, line, &ev);
}

static void ev_digits(unsigned int line, const char *digits)
{
	for (; *digits; digits++)
		ev_digit(line, *digits);
}

static void ev_tone(unsigned int line, uint16_t code, bool end)
{
	struct proto_line_event ev = { .type = PROTO_EV_TONE };

	ev.u.tone.code = code;
	ev.u.tone.end = end;
	mgcp_ep_line_event(&gw, line, &ev);
}

static unsigned int hop_count(enum phf_entry entry)
{
	unsigned int i, n = 0;

	for (i = hop_mark; i < phf.nhop; i++)
		n += phf.hop[i].entry == entry;

	return n;
}

static struct phf_hop hop_none;

/* The last call of entry since the mark; a case that reads a call the
 * code did not make fails at the reading line. */
#define hop_last(entry)	hop_last_at((entry), __LINE__)

static const struct phf_hop *hop_last_at(enum phf_entry entry, int line)
{
	unsigned int i;

	for (i = phf.nhop; i > hop_mark; i--) {
		if (phf.hop[i - 1].entry == entry)
			return &phf.hop[i - 1];
	}
	tu_check(false, "hop_last: the host entry was not called", __FILE__,
		 line);
	memset(&hop_none, 0, sizeof(hop_none));

	return &hop_none;
}

static int hop_index(enum phf_entry entry)
{
	unsigned int i;

	for (i = hop_mark; i < phf.nhop; i++) {
		if (phf.hop[i].entry == entry)
			return (int)i;
	}

	return -1;
}

/* The streams of the calls of entry since the mark, as "0 1 0". */
static const char *hop_streams(enum phf_entry entry)
{
	static char text[64];
	size_t len = 0;
	unsigned int i;

	text[0] = '\0';
	for (i = hop_mark; i < phf.nhop && len < sizeof(text); i++) {
		if (phf.hop[i].entry != entry)
			continue;
		len += (size_t)snprintf(text + len, sizeof(text) - len, "%s%u",
					len ? " " : "", phf.hop[i].stream);
	}

	return text;
}

/* The restart of the fixture: the delay runs out and the call agent
 * answers the RSIP with 200. */
static void gw_up(void)
{
	mgcp_ep_start(&gw);
	fake_advance(10001);
	ca_answer(sent("RSIP"), 200, NULL);
}

static void up(const struct fx *f)
{
	fixture_start(f);
	gw_up();
	mark_set();
}

static const struct fx fx_rfc = { CFG_MGCP_RFC3435, false, NULL };
static const struct fx fx_ncs = { CFG_MGCP_NCS, false, NULL };
static const struct fx fx_loop = { CFG_MGCP_RFC3435, true, NULL };
static const struct fx fx_none = { CFG_MGCP_RFC3435, false, "" };

/* An off-hook endpoint 1, with its persistent NTFY answered. */
static void offhook_up(const struct fx *f)
{
	up(f);
	ev_hook(0, HAL_HOOK_SEIZE);
	ca_answer(sent("NTFY"), 200, NULL);
	mark_set();
}

#define SDP_ANSWER							\
	"v=0\r\n"							\
	"o=- 1 1 IN IP4 192.0.2.50\r\n"					\
	"s=-\r\n"							\
	"c=IN IP4 192.0.2.50\r\n"					\
	"t=0 0\r\n"							\
	"m=audio 30000 RTP/AVP 0 101\r\n"				\
	"a=rtpmap:101 telephone-event/8000\r\n"

#define SDP_HOLD							\
	"v=0\r\n"							\
	"o=- 1 1 IN IP4 192.0.2.50\r\n"					\
	"s=-\r\n"							\
	"c=IN IP4 0.0.0.0\r\n"						\
	"t=0 0\r\n"							\
	"m=audio 30000 RTP/AVP 0\r\n"

#define SDP_PCMA							\
	"v=0\r\n"							\
	"o=- 1 2 IN IP4 192.0.2.50\r\n"					\
	"s=-\r\n"							\
	"c=IN IP4 192.0.2.50\r\n"					\
	"t=0 0\r\n"							\
	"m=audio 30002 RTP/AVP 8\r\n"

#define SDP_T38								\
	"v=0\r\n"							\
	"o=- 1 3 IN IP4 192.0.2.50\r\n"					\
	"s=-\r\n"							\
	"c=IN IP4 192.0.2.50\r\n"					\
	"t=0 0\r\n"							\
	"m=audio 0 RTP/AVP 0\r\n"					\
	"m=image 30004 udptl t38\r\n"

/* the connection id of a CRCX response */
static void conn_id_of(const char *resp, char *out, size_t olen)
{
	const char *i = strstr(resp, "\r\nI: ");
	size_t n = 0;

	out[0] = '\0';
	if (!i)
		return;
	i += 5;
	while (i[n] && i[n] != '\r' && n + 1 < olen)
		n++;
	memcpy(out, i, n);
	out[n] = '\0';
}

/* A connection in sendrecv on endpoint 1 through the offer path. */
static void call_up(char *id, size_t olen)
{
	char rest[1024];
	const char *r;

	r = ca_cmd("CRCX", EP1, "C: A1\r\nM: recvonly\r\nL: a:PCMU\r\n");
	conn_id_of(r, id, olen);
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\nM: sendrecv\r\n\r\n"
		 SDP_ANSWER, id);
	ca_cmd("MDCX", EP1, rest);
	mark_set();
}

/* ------------------------------------------------------- the package tables */

static void sig_tests(void)
{
	struct mgcp_event ev;
	struct mgcp_ev_pat p;
	struct mgcp_obs o;
	struct proto_cid cid;
	const struct mgcp_sig_def *def;
	uint32_t code = 0;

	CASE("sig: L/dl is a TO signal of 16 s that plays the dial tone");
	mgcp_event_parse(sip_str_cstr("L/dl"), sip_str_cstr("L"), &ev);
	def = mgcp_sig_lookup(CFG_MGCP_RFC3435, &ev, &code);
	CHECK(def && def->type == MGCP_SIG_TO && def->to_ms == 16000);
	CHECK(def && def->prim == MGCP_PRIM_TONE && def->arg == HAL_TONE_DIAL);

	CASE("sig: an unknown package gets 518, an unknown name 522");
	mgcp_event_parse(sip_str_cstr("X/dl"), sip_str_cstr("L"), &ev);
	CHECK(!mgcp_sig_lookup(CFG_MGCP_RFC3435, &ev, &code) && code == 518);
	mgcp_event_parse(sip_str_cstr("L/zz"), sip_str_cstr("L"), &ev);
	CHECK(!mgcp_sig_lookup(CFG_MGCP_RFC3435, &ev, &code) && code == 522);

	CASE("sig: ncs knows L alone; G/rt gets 518 and L/rt is a ringback");
	mgcp_event_parse(sip_str_cstr("G/rt"), sip_str_cstr("L"), &ev);
	CHECK(!mgcp_sig_lookup(CFG_MGCP_NCS, &ev, &code) && code == 518);
	mgcp_event_parse(sip_str_cstr("L/rt"), sip_str_cstr("L"), &ev);
	def = mgcp_sig_lookup(CFG_MGCP_NCS, &ev, &code);
	CHECK(def && def->arg == HAL_TONE_RINGBACK && def->to_ms == 180000);

	CASE("sig: ncs has no lsa and no plain wt");
	mgcp_event_parse(sip_str_cstr("L/lsa"), sip_str_cstr("L"), &ev);
	CHECK(!mgcp_sig_lookup(CFG_MGCP_NCS, &ev, &code) && code == 522);
	mgcp_event_parse(sip_str_cstr("L/wt"), sip_str_cstr("L"), &ev);
	CHECK(!mgcp_sig_lookup(CFG_MGCP_NCS, &ev, &code) && code == 522);

	CASE("sig: rep wins over to for a ring signal");
	{
		struct mgcp_sig_params sp;

		mgcp_event_parse(sip_str_cstr("L/rg(rep=6,to=12000)"),
				 sip_str_cstr("L"), &ev);
		def = mgcp_sig_lookup(CFG_MGCP_RFC3435, &ev, &code);
		CHECK(def && mgcp_sig_params_parse(def, &ev, &sp));
		CHECK_UINT(mgcp_sig_timeout(def, &sp, 6000), 36000);
	}

	CASE("sig: ci strips the time to digits and marks P private");
	CHECK(mgcp_sig_cid_parse(sip_str_cstr("09/14/17/26, P, \"John Doe\""),
				 &cid));
	CHECK(!strcmp(cid.datetime, "09141726"));
	CHECK(cid.number_private && !cid.number[0]);
	CHECK(!strcmp(cid.name, "John Doe") && cid.present);

	CASE("sig: ci keeps empty fields and refuses two fields");
	CHECK(mgcp_sig_cid_parse(sip_str_cstr(", \"555 1212\","), &cid));
	CHECK(!strcmp(cid.number, "5551212") && !cid.datetime[0]);
	CHECK(!mgcp_sig_cid_parse(sip_str_cstr("09/14/17/26, 5551212"),
				  &cid));

	CASE("sig: ci keeps the head of a quoted field of 600 characters");
	{
		char text[700];

		memset(text, 0, sizeof(text));
		memcpy(text, ", 5551212, \"", 12);
		memset(text + 12, 'N', 600);
		text[612] = '"';
		CHECK(mgcp_sig_cid_parse(sip_str_cstr(text), &cid));
		CHECK_UINT(strlen(cid.name), sizeof(cid.name) - 1);
		CHECK(cid.name[0] == 'N' && !strcmp(cid.number, "5551212"));
	}

	CASE("sig: a digit range with T compiles to a digit set");
	mgcp_event_parse(sip_str_cstr("D/[0-9#*T]"), sip_str_cstr("L"), &ev);
	CHECK(mgcp_ev_pat_compile(CFG_MGCP_RFC3435, &ev, &p) == 0);
	CHECK(p.kind == MGCP_EVK_DIGIT && p.timer && p.digits == 0x0fff);

	CASE("sig: an unqualified digit range takes the digit package");
	mgcp_event_parse(sip_str_cstr("[0-9T]"), sip_str_cstr("L"), &ev);
	CHECK(mgcp_ev_pat_compile(CFG_MGCP_RFC3435, &ev, &p) == 0);
	CHECK(p.pkg == MGCP_PKG_D);
	CHECK(mgcp_ev_pat_compile(CFG_MGCP_NCS, &ev, &p) == 0);
	CHECK(p.pkg == MGCP_PKG_L);

	CASE("sig: a defined event the gateway does not detect gets 512");
	mgcp_event_parse(sip_str_cstr("G/ld"), sip_str_cstr("L"), &ev);
	CHECK(mgcp_ev_pat_compile(CFG_MGCP_RFC3435, &ev, &p) == 512);

	CASE("sig: CED and ANSam report mt, V.21 and CNG report ft");
	{
		struct proto_line_event le = { .type = PROTO_EV_TONE };

		le.u.tone.code = HAL_TONE_DET_CED;
		CHECK(mgcp_obs_of_line_event(&le, &o) &&
		      o.kind == MGCP_EVK_MT);
		le.u.tone.code = HAL_TONE_DET_ANSAM;
		CHECK(mgcp_obs_of_line_event(&le, &o) &&
		      o.kind == MGCP_EVK_MT);
		le.u.tone.code = HAL_TONE_DET_V21_PREAMBLE;
		CHECK(mgcp_obs_of_line_event(&le, &o) &&
		      o.kind == MGCP_EVK_FT);
		le.u.tone.code = HAL_TONE_DET_CNG;
		CHECK(mgcp_obs_of_line_event(&le, &o) &&
		      o.kind == MGCP_EVK_FT);
		le.u.tone.end = 1;
		CHECK(!mgcp_obs_of_line_event(&le, &o));
	}

	CASE("sig: an oc names its signal in the package of that signal");
	{
		char buf[64];
		struct sip_buf b;

		memset(&o, 0, sizeof(o));
		o.kind = MGCP_EVK_OC;
		str_copy(o.sig, sizeof(o.sig), "G/rt");
		sip_buf_init(&b, buf, sizeof(buf));
		CHECK(mgcp_obs_write(CFG_MGCP_RFC3435, &o, &b));
		CHECK(!strcmp(buf, "G/oc(G/rt)"));
	}
}

/* ------------------------------------------------------------- restart */

static void restart_tests(void)
{
	const char *r;
	uint32_t delay;

	CASE("restart: RSIP restart for every endpoint after the random delay");
	fixture_start(&fx_rfc);
	/* the fake entropy fills bytes 1, 2, 3, 4 */
	delay = 0x04030201u % 10001u;
	mgcp_ep_start(&gw);
	fake_advance(delay - 1);
	CHECK_UINT(fk.sends, 0);
	fake_advance(1);
	r = sent("RSIP");
	CHECK(has(r, "RSIP ") && has(r, " aaln/*@gw.example.net MGCP 1.0"));
	CHECK(has(r, "RM: restart"));
	CHECK(!strcmp(mgcp_ep_state_name(&gw, 0), "restarting"));
	ca_answer(r, 200, NULL);
	CHECK(!strcmp(mgcp_ep_state_name(&gw, 0), "idle"));
	CHECK(!strcmp(mgcp_ep_state_name(&gw, 1), "idle"));
	fixture_stop();

	CASE("restart: a 200 with N: moves the notified entity");
	fixture_start(&fx_rfc);
	mgcp_ep_start(&gw);
	fake_advance(10001);
	ca_answer(sent("RSIP"), 200, "N: ca2@[192.0.2.2]:2727\r\n");
	mark_set();
	ev_hook(0, HAL_HOOK_SEIZE);
	CHECK(sent_rec("NTFY") && !strcmp(sent_rec("NTFY")->host,
					  "192.0.2.2"));
	CHECK(!strcmp(fk.entity, "ca2@[192.0.2.2]:2727"));
	fixture_stop();

	CASE("restart: a 521 with N: sends a new RSIP to the new entity");
	fixture_start(&fx_rfc);
	mgcp_ep_start(&gw);
	fake_advance(10001);
	r = sent("RSIP");
	mark_set();
	ca_answer(r, 521, "N: ca2@[192.0.2.2]:2727\r\n");
	CHECK(sent_rec("RSIP") && !strcmp(sent_rec("RSIP")->host,
					  "192.0.2.2"));
	CHECK(txid_of(sent("RSIP")) != txid_of(r));
	CHECK(!strcmp(mgcp_ep_state_name(&gw, 0), "restarting"));
	fixture_stop();

	CASE("restart: another 5xx stops the procedure until a command");
	fixture_start(&fx_rfc);
	mgcp_ep_start(&gw);
	fake_advance(10001);
	ca_answer(sent("RSIP"), 510, NULL);
	mark_set();
	fake_advance(60000);
	CHECK_UINT(sent_count("RSIP"), 0);
	r = ca_cmd("RQNT", EP1, "X: 1\r\n");
	CHECK_UINT(code_of(r), 405);
	CHECK_UINT(sent_count("RSIP"), 1);
	fixture_stop();

	CASE("restart: Max2 on the RSIP enters the disconnected procedure");
	fixture_start(&fx_rfc);
	mgcp_ep_start(&gw);
	fake_advance(10001);
	r = sent("RSIP");
	fake_advance(30000);
	CHECK(mgcp_txn_disc_active(&mgr));
	mark_set();
	fake_advance(15000);
	/* rfc3435 4.4.6: the procedure sends restart, not disconnected */
	r = sent("RSIP");
	CHECK(has(r, "RM: restart") && has(r, "aaln/*@gw.example.net"));
	ca_answer(r, 200, NULL);
	CHECK(!mgcp_txn_disc_active(&mgr));
	CHECK(!strcmp(mgcp_ep_state_name(&gw, 0), "idle"));
	fixture_stop();

	CASE("restart: an off-hook in the delay sends the RSIP at once");
	fixture_start(&fx_rfc);
	mgcp_ep_start(&gw);
	ev_hook(0, HAL_HOOK_SEIZE);
	r = sent("RSIP");
	CHECK(has(r, "RM: restart"));
	CHECK_UINT(sent_count("NTFY"), 0);
	mark_set();
	ca_answer(r, 200, NULL);
	/* RFC 3435 4.4.1: the persistent hd under the request id 0 */
	r = sent("NTFY");
	CHECK(has(r, "NTFY ") && has(r, EP1) && has(r, "X: 0\r\n") &&
	      has(r, "O: L/hd\r\n"));
	fixture_stop();

	CASE("restarting: a non-audit command gets 405 under rfc3435");
	fixture_start(&fx_rfc);
	mgcp_ep_start(&gw);
	r = ca_cmd("RQNT", EP1, "X: 1\r\nR: L/hd\r\n");
	CHECK_UINT(code_of(r), 405);
	CHECK(!strncmp(r, "405", 3));
	CHECK_UINT(sent_count("RSIP"), 1);
	fixture_stop();

	CASE("restarting: ncs answers 520 behind the RSIP in one datagram");
	fixture_start(&fx_ncs);
	mgcp_ep_start(&gw);
	r = ca_cmd("RQNT", EP1, "X: 1\r\nR: L/hd\r\n");
	CHECK(!strncmp(r, "RSIP ", 5) && has(r, "RM: restart"));
	CHECK(has(r, "\r\n.\r\n520 "));
	CHECK_UINT(code_of(r), 520);
	fixture_stop();

	CASE("restarting: an audit gets its answer");
	fixture_start(&fx_rfc);
	mgcp_ep_start(&gw);
	r = ca_cmd("AUEP", EP1, "F: RM\r\n");
	CHECK(!strncmp(r, "200", 3) && has(r, "RM: restart"));
	fixture_stop();
}

/* ------------------------------------------------------------- outbound */

static const char *rqnt_digits(const char *map, const char *events)
{
	char rest[512];

	snprintf(rest, sizeof(rest), "X: 0A\r\nR: L/hu(N), %s\r\nS: L/dl\r\n"
		 "D: (%s)\r\n", events, map);

	return ca_cmd("RQNT", EP1, rest);
}

static void outbound_tests(void)
{
	const char *r;
	uint64_t due;

	CASE("outbound: the seize sends NTFY L/hd under the persistent mask");
	up(&fx_rfc);
	ev_hook(0, HAL_HOOK_SEIZE);
	r = sent("NTFY");
	CHECK(has(r, "X: 0\r\n") && has(r, "O: L/hd\r\n"));
	fixture_stop();

	CASE("outbound: no persistent event and no request sends nothing");
	up(&fx_none);
	ev_hook(0, HAL_HOOK_SEIZE);
	CHECK_UINT(sent_count("NTFY"), 0);
	fixture_stop();

	CASE("outbound: RQNT arms the detector, then plays the dial tone");
	offhook_up(&fx_rfc);
	r = rqnt_digits("xxxx", "D/[0-9#*T](D)");
	CHECK(!strncmp(r, "200", 3));
	CHECK(hop_index(PHF_DETECTOR_ARM) >= 0 &&
	      hop_index(PHF_DETECTOR_ARM) < hop_index(PHF_TONE_PLAY));
	CHECK(hop_last(PHF_DETECTOR_ARM)->val & HAL_DETECT_DTMF);
	CHECK_UINT(hop_last(PHF_TONE_PLAY)->val, HAL_TONE_DIAL);
	fixture_stop();

	CASE("outbound: a perfect match sends the digits in O: order");
	offhook_up(&fx_rfc);
	rqnt_digits("xxxx", "D/[0-9#*T](D)");
	mark_set();
	ev_digits(0, "123");
	CHECK_UINT(sent_count("NTFY"), 0);
	due = fake_timer_due(&gw.ep[0].digit_timer);
	CHECK_UINT(due, fk.now + 16000);
	ev_digit(0, '4');
	r = sent("NTFY");
	CHECK(has(r, "X: 0A\r\n") && has(r, "O: D/1,D/2,D/3,D/4\r\n"));
	fixture_stop();

	CASE("outbound: WAIT_SHORT runs the 4 s timer and sends the T");
	offhook_up(&fx_rfc);
	rqnt_digits("xxT|xxxx", "D/[0-9#*T](D)");
	mark_set();
	ev_digits(0, "12");
	CHECK_UINT(fake_timer_due(&gw.ep[0].digit_timer), fk.now + 4000);
	fake_advance(3999);
	CHECK_UINT(sent_count("NTFY"), 0);
	fake_advance(1);
	CHECK(has(sent("NTFY"), "O: D/1,D/2,D/T\r\n"));
	fixture_stop();

	CASE("outbound: WAIT_LONG runs the 16 s timer");
	offhook_up(&fx_rfc);
	rqnt_digits("xxxx", "D/[0-9#*T](D)");
	mark_set();
	ev_digit(0, '5');
	fake_advance(15999);
	CHECK_UINT(sent_count("NTFY"), 0);
	fake_advance(1);
	CHECK(has(sent("NTFY"), "O: D/5,D/T\r\n"));
	fixture_stop();

	CASE("outbound: 9T11 appends the T and dials 911 after it");
	offhook_up(&fx_rfc);
	rqnt_digits("9T11", "D/[0-9#*T](D)");
	mark_set();
	ev_digit(0, '9');
	CHECK_UINT(fake_timer_due(&gw.ep[0].digit_timer), fk.now + 16000);
	fake_advance(16000);
	CHECK_UINT(sent_count("NTFY"), 0);
	ev_digits(0, "11");
	CHECK(has(sent("NTFY"), "O: D/9,D/T,D/1,D/1\r\n"));
	fixture_stop();

	CASE("outbound: an impossible match sends the digit that caused it");
	offhook_up(&fx_rfc);
	rqnt_digits("xxxx", "D/[0-9#*T](D)");
	mark_set();
	ev_digit(0, '*');
	CHECK(has(sent("NTFY"), "O: D/*\r\n"));
	fixture_stop();

	CASE("outbound: no T in the request runs no interdigit timer");
	offhook_up(&fx_rfc);
	rqnt_digits("xxxx", "D/[0-9#*](D)");
	ev_digit(0, '1');
	CHECK_UINT(fake_timer_due(&gw.ep[0].digit_timer), 0);
	fixture_stop();

	CASE("outbound: T without the D action runs now; a digit stops it");
	offhook_up(&fx_rfc);
	ca_cmd("RQNT", EP1, "X: 0B\r\nR: D/[0-9](N), D/T(N)\r\n");
	CHECK_UINT(fake_timer_due(&gw.ep[0].digit_timer), fk.now + 4000);
	mark_set();
	ev_digit(0, '7');
	CHECK_UINT(fake_timer_due(&gw.ep[0].digit_timer), 0);
	CHECK(has(sent("NTFY"), "O: D/7\r\n"));
	fixture_stop();

	CASE("outbound: the first digit stops the dial tone");
	offhook_up(&fx_rfc);
	rqnt_digits("xxxx", "D/[0-9#*T](D)");
	mark_set();
	ev_digit(0, '1');
	CHECK_UINT(hop_count(PHF_TONE_STOP), 1);
	fixture_stop();

	CASE("outbound: a digit with K keeps the dial tone");
	offhook_up(&fx_rfc);
	rqnt_digits("xxxx", "D/[0-9#*T](D,K)");
	mark_set();
	ev_digit(0, '1');
	CHECK_UINT(hop_count(PHF_TONE_STOP), 0);
	fixture_stop();

	CASE("outbound: the dial tone times out with oc when oc is asked");
	offhook_up(&fx_rfc);
	ca_cmd("RQNT", EP1, "X: 0C\r\nR: L/oc(N)\r\nS: L/dl\r\n");
	mark_set();
	fake_advance(16000);
	CHECK_UINT(hop_count(PHF_TONE_STOP), 1);
	CHECK(has(sent("NTFY"), "O: L/oc(L/dl)\r\n"));
	fixture_stop();

	CASE("ncs: the version token, L/ digits and L/oc on the wire");
	offhook_up(&fx_ncs);
	ca_cmd("RQNT", EP1, "X: 0D\r\nR: [0-9T](D)\r\nS: dl\r\nD: (xx)\r\n");
	mark_set();
	ev_digits(0, "42");
	r = sent("NTFY");
	CHECK(has(r, " " EP1 " MGCP 1.0 NCS 1.0\r\n"));
	CHECK(has(r, "O: L/4,L/2\r\n"));
	fixture_stop();
}

/* ------------------------------------------------------------ connections */

static void conn_tests(void)
{
	char id[MGCP_ID_MAX], id2[MGCP_ID_MAX], rest[1024];
	const char *r;

	CASE("crcx: the offer path writes the descriptor and starts nothing");
	offhook_up(&fx_rfc);
	r = ca_cmd("CRCX", EP1, "C: A1\r\nM: recvonly\r\n"
		   "L: p:20, a:PCMA;PCMU\r\n");
	CHECK(!strncmp(r, "200", 3));
	conn_id_of(r, id, sizeof(id));
	CHECK_UINT(strlen(id), 8);
	CHECK(has(r, "\r\n\r\nv=0\r\n"));
	CHECK(has(r, "m=audio 16384 RTP/AVP 8 0"));
	CHECK(has(r, "c=IN IP4 192.0.2.10"));
	CHECK(!has(r, "a=recvonly") && !has(r, "a=sendrecv") &&
	      !has(r, "a=sendonly") && !has(r, "a=inactive"));
	CHECK_UINT(hop_count(PHF_MEDIA_CONFIG_SET), 0);
	CHECK_UINT(hop_count(PHF_MEDIA_START), 0);
	fixture_stop();

	CASE("mdcx: the answer path programs and starts the stream");
	offhook_up(&fx_rfc);
	r = ca_cmd("CRCX", EP1, "C: A1\r\nM: recvonly\r\nL: a:PCMU\r\n");
	conn_id_of(r, id, sizeof(id));
	mark_set();
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\nM: sendrecv\r\n\r\n"
		 SDP_ANSWER, id);
	r = ca_cmd("MDCX", EP1, rest);
	CHECK_UINT(code_of(r), 200);
	CHECK(!has(r, "v=0"));
	CHECK_UINT(hop_count(PHF_MEDIA_CONFIG_SET), 1);
	CHECK_UINT(hop_last(PHF_MEDIA_CONFIG_SET)->media.remote.port, 30000);
	CHECK_UINT(hop_last(PHF_MEDIA_CONFIG_SET)->media.direction,
		   HAL_MEDIA_SENDRECV);
	CHECK_UINT(hop_count(PHF_MEDIA_START), 1);
	CHECK(gw.ep[0].conn[0].sdp.committed.valid);
	CHECK(!gw.ep[0].conn[0].sdp.offer_pending);
	fixture_stop();

	CASE("mdcx: a new offer changes the codec and returns a descriptor");
	offhook_up(&fx_rfc);
	call_up(id, sizeof(id));
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\n\r\n" SDP_PCMA, id);
	r = ca_cmd("MDCX", EP1, rest);
	CHECK_UINT(code_of(r), 200);
	CHECK(has(r, "m=audio 16384 RTP/AVP 8"));
	CHECK_UINT(hop_last(PHF_MEDIA_CONFIG_SET)->media.codec,
		   HAL_CODEC_PCMA);
	CHECK_UINT(hop_last(PHF_MEDIA_CONFIG_SET)->media.remote.port, 30002);
	fixture_stop();

	CASE("mdcx: a T.38 offer moves the stream to UDPTL");
	offhook_up(&fx_rfc);
	call_up(id, sizeof(id));
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\n\r\n" SDP_T38, id);
	r = ca_cmd("MDCX", EP1, rest);
	CHECK_UINT(code_of(r), 200);
	CHECK(has(r, "m=image 16384 udptl t38"));
	CHECK(hop_index(PHF_MEDIA_STOP) >= 0 &&
	      hop_index(PHF_MEDIA_STOP) < hop_index(PHF_MEDIA_CONFIG_SET));
	CHECK_UINT(hop_last(PHF_MEDIA_CONFIG_SET)->media.codec,
		   HAL_CODEC_T38);
	CHECK_UINT(hop_count(PHF_MEDIA_START), 1);
	fixture_stop();

	CASE("mdcx: a refused T.38 offer gets 505 and keeps the call");
	offhook_up(&fx_rfc);
	phf.hal_features &= ~(uint32_t)HAL_FEAT_T38;
	call_up(id, sizeof(id));
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\n\r\n" SDP_T38, id);
	r = ca_cmd("MDCX", EP1, rest);
	CHECK_UINT(code_of(r), 505);
	CHECK_UINT(hop_count(PHF_MEDIA_CONFIG_SET), 0);
	CHECK_UINT(gw.ep[0].conn[0].sdp.committed.codec.id, SDP_CODEC_PCMU);
	fixture_stop();

	CASE("mdcx: a mode change alone, no descriptor and no tone (hold)");
	offhook_up(&fx_rfc);
	call_up(id, sizeof(id));
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\nM: inactive\r\n", id);
	r = ca_cmd("MDCX", EP1, rest);
	CHECK_UINT(code_of(r), 200);
	CHECK(!has(r, "v=0"));
	CHECK_UINT(hop_count(PHF_MEDIA_CONFIG_SET), 1);
	CHECK_UINT(hop_last(PHF_MEDIA_CONFIG_SET)->media.direction,
		   HAL_MEDIA_INACTIVE);
	CHECK_UINT(hop_count(PHF_TONE_PLAY), 0);
	fixture_stop();

	CASE("mdcx: an unknown I: gets 515 and a wrong C: gets 516");
	offhook_up(&fx_rfc);
	call_up(id, sizeof(id));
	r = ca_cmd("MDCX", EP1, "C: A1\r\nI: 1234ABCD\r\nM: sendrecv\r\n");
	CHECK_UINT(code_of(r), 515);
	snprintf(rest, sizeof(rest), "C: B2\r\nI: %s\r\nM: sendrecv\r\n", id);
	r = ca_cmd("MDCX", EP1, rest);
	CHECK_UINT(code_of(r), 516);
	fixture_stop();

	CASE("dlcx: P: comes from the stream counters");
	offhook_up(&fx_rfc);
	call_up(id, sizeof(id));
	phf.stats.packets_sent = 10;
	phf.stats.bytes_sent = 1600;
	phf.stats.packets_recv = 12;
	phf.stats.bytes_recv = 1920;
	phf.stats.packets_lost = 1;
	phf.stats.jitter_avg = 3;
	phf.stats.rtt_avg = 40;
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\n", id);
	r = ca_cmd("DLCX", EP1, rest);
	CHECK_UINT(code_of(r), 250);
	CHECK(has(r, "P: PS=10, OS=1600, PR=12, OR=1920, PL=1, JI=3, LA=20"));
	CHECK(hop_index(PHF_STREAM_STATS_GET) >= 0 &&
	      hop_index(PHF_STREAM_STATS_GET) < hop_index(PHF_MEDIA_STOP));
	CHECK(!gw.ep[0].conn[0].used);
	fixture_stop();

	CASE("dlcx: P: is left out when the backend has no counters");
	offhook_up(&fx_rfc);
	call_up(id, sizeof(id));
	phf.stats_status = HAL_UNSUPPORTED;
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\n", id);
	r = ca_cmd("DLCX", EP1, rest);
	CHECK_UINT(code_of(r), 250);
	CHECK(!has(r, "P:"));
	fixture_stop();

	CASE("dlcx: C: alone deletes the connections of that call");
	offhook_up(&fx_rfc);
	ca_cmd("CRCX", EP1, "C: A1\r\nM: inactive\r\n");
	ca_cmd("CRCX", EP1, "C: B2\r\nM: inactive\r\n");
	r = ca_cmd("DLCX", EP1, "C: A1\r\n");
	CHECK_UINT(code_of(r), 250);
	CHECK(!has(r, "P:"));
	CHECK(!gw.ep[0].conn[0].used && gw.ep[0].conn[1].used);
	fixture_stop();

	CASE("dlcx: no C: and no I: deletes every connection of the endpoint");
	offhook_up(&fx_rfc);
	ca_cmd("CRCX", EP1, "C: A1\r\nM: inactive\r\n");
	ca_cmd("CRCX", EP1, "C: B2\r\nM: inactive\r\n");
	r = ca_cmd("DLCX", EP1, NULL);
	CHECK_UINT(code_of(r), 250);
	CHECK(!gw.ep[0].conn[0].used && !gw.ep[0].conn[1].used);
	fixture_stop();

	CASE("crcx: recvonly with a remote description answers and starts");
	up(&fx_rfc);
	r = ca_cmd("CRCX", EP1, "C: A1\r\nM: recvonly\r\n\r\n" SDP_ANSWER);
	CHECK_UINT(code_of(r), 200);
	CHECK(has(r, "m=audio 16384 RTP/AVP 0"));
	CHECK_UINT(hop_last(PHF_MEDIA_CONFIG_SET)->media.direction,
		   HAL_MEDIA_RECVONLY);
	CHECK_UINT(hop_count(PHF_MEDIA_START), 1);
	fixture_stop();

	CASE("crcx: inactive with a remote description configures, no start");
	up(&fx_rfc);
	r = ca_cmd("CRCX", EP1, "C: A1\r\nM: inactive\r\n\r\n" SDP_ANSWER);
	CHECK_UINT(code_of(r), 200);
	CHECK_UINT(hop_count(PHF_MEDIA_CONFIG_SET), 1);
	CHECK_UINT(hop_count(PHF_MEDIA_START), 0);
	fixture_stop();

	CASE("crcx: sendrecv without a remote description gets 527");
	up(&fx_rfc);
	CHECK_UINT(code_of(ca_cmd("CRCX", EP1, "C: A1\r\nM: sendrecv\r\n")),
		   527);
	CHECK_UINT(code_of(ca_cmd("CRCX", EP1, "C: A1\r\nM: sendonly\r\n")),
		   527);
	CHECK(!gw.ep[0].conn[0].used);
	fixture_stop();

	CASE("crcx: an unsupported mode gets 517 before 527");
	up(&fx_rfc);
	CHECK_UINT(code_of(ca_cmd("CRCX", EP1, "C: A1\r\nM: loopback\r\n")),
		   517);
	CHECK_UINT(code_of(ca_cmd("CRCX", EP1, "C: A1\r\nM: netwtest\r\n")),
		   517);
	CHECK_UINT(code_of(ca_cmd("CRCX", EP1, "C: A1\r\nM: confrnce\r\n")),
		   517);
	fixture_stop();

	CASE("crcx: a third connection gets 540, 502 under ncs");
	up(&fx_rfc);
	ca_cmd("CRCX", EP1, "C: A1\r\nM: inactive\r\n");
	ca_cmd("CRCX", EP1, "C: A1\r\nM: inactive\r\n");
	CHECK_UINT(code_of(ca_cmd("CRCX", EP1, "C: A1\r\nM: inactive\r\n")),
		   540);
	fixture_stop();
	up(&fx_ncs);
	ca_cmd("CRCX", EP1, "C: A1\r\nM: inactive\r\n");
	ca_cmd("CRCX", EP1, "C: A1\r\nM: inactive\r\n");
	CHECK_UINT(code_of(ca_cmd("CRCX", EP1, "C: A1\r\nM: inactive\r\n")),
		   502);
	fixture_stop();

	CASE("crcx: L: codes 534, 532, 525 and the packet time");
	up(&fx_rfc);
	CHECK_UINT(code_of(ca_cmd("CRCX", EP1, "C: A1\r\nM: recvonly\r\n"
				  "L: a:G723\r\n")), 534);
	CHECK_UINT(code_of(ca_cmd("CRCX", EP1, "C: A1\r\nM: recvonly\r\n"
				  "L: e:maybe\r\n")), 532);
	CHECK_UINT(code_of(ca_cmd("CRCX", EP1, "C: A1\r\nM: recvonly\r\n"
				  "L: x+vendor:1\r\n")), 525);
	CHECK_UINT(code_of(ca_cmd("CRCX", EP1, "C: A1\r\nM: recvonly\r\n"
				  "L: p:25\r\n")), 535);
	r = ca_cmd("CRCX", EP1, "C: A1\r\nM: recvonly\r\nL: p:10-15\r\n");
	CHECK(has(r, "a=ptime:10"));
	fixture_stop();
	up(&fx_ncs);
	CHECK_UINT(code_of(ca_cmd("CRCX", EP1, "C: A1\r\nM: recvonly\r\n"
				  "L: p:25\r\n")), 532);
	fixture_stop();

	CASE("crcx: a remote description that does not parse gets 509");
	up(&fx_rfc);
	CHECK_UINT(code_of(ca_cmd("CRCX", EP1, "C: A1\r\nM: recvonly\r\n\r\n"
				  "x=garbage\r\n")), 509);
	fixture_stop();
	up(&fx_ncs);
	CHECK_UINT(code_of(ca_cmd("CRCX", EP1, "C: A1\r\nM: recvonly\r\n\r\n"
				  "x=garbage\r\n")), 505);
	fixture_stop();

	CASE("confrnce: both connections in confrnce turn the mixer on");
	offhook_up(&fx_rfc);
	phf.hal_features |= HAL_FEAT_CONF;
	call_up(id, sizeof(id));
	r = ca_cmd("CRCX", EP1, "C: B2\r\nM: sendrecv\r\n\r\n" SDP_PCMA);
	conn_id_of(r, id2, sizeof(id2));
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\nM: confrnce\r\n", id);
	ca_cmd("MDCX", EP1, rest);
	CHECK_UINT(hop_count(PHF_CONF_SET), 0);
	snprintf(rest, sizeof(rest), "C: B2\r\nI: %s\r\nM: confrnce\r\n", id2);
	ca_cmd("MDCX", EP1, rest);
	CHECK_UINT(hop_count(PHF_CONF_SET), 1);
	CHECK(hop_last(PHF_CONF_SET)->on);
	mark_set();
	snprintf(rest, sizeof(rest), "C: B2\r\nI: %s\r\nM: sendrecv\r\n", id2);
	ca_cmd("MDCX", EP1, rest);
	CHECK_UINT(hop_count(PHF_CONF_SET), 1);
	CHECK(!hop_last(PHF_CONF_SET)->on);
	fixture_stop();

	CASE("confrnce: without the mixer feature it gets 517");
	offhook_up(&fx_rfc);
	call_up(id, sizeof(id));
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\nM: confrnce\r\n", id);
	CHECK_UINT(code_of(ca_cmd("MDCX", EP1, rest)), 517);
	fixture_stop();

	CASE("mdcx: successive requests on one connection keep one stream");
	offhook_up(&fx_rfc);
	call_up(id, sizeof(id));
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\nM: sendonly\r\n", id);
	CHECK_UINT(code_of(ca_cmd("MDCX", EP1, rest)), 200);
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\nM: sendrecv\r\n\r\n"
		 SDP_PCMA, id);
	CHECK_UINT(code_of(ca_cmd("MDCX", EP1, rest)), 200);
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\nL: a:PCMU\r\n", id);
	r = ca_cmd("MDCX", EP1, rest);
	CHECK_UINT(code_of(r), 534);
	CHECK_UINT(hop_count(PHF_MEDIA_START), 0);
	CHECK_UINT(hop_count(PHF_MEDIA_STOP), 0);
	CHECK_UINT(gw.ep[0].conn[0].stream, 0);
	fixture_stop();
}

/* ------------------------------------------------------- inbound signals */

static void inbound_tests(void)
{
	char id[MGCP_ID_MAX], id2[MGCP_ID_MAX], rest[1024];
	const char *r;

	CASE("inbound: rg rings the template the number match gives");
	up(&fx_rfc);
	phf.ring_template = 3;
	r = ca_cmd("RQNT", EP1, "X: 10\r\nR: L/hd(N)\r\nS: L/rg\r\n");
	CHECK_UINT(code_of(r), 200);
	CHECK_UINT(hop_count(PHF_RING_START), 1);
	CHECK_UINT(hop_last(PHF_RING_START)->val, 3);
	CHECK_UINT(hop_last(PHF_RING_START)->duration_ms, 0);
	CHECK(!strcmp(mgcp_ep_state_name(&gw, 0), "ringing"));
	CHECK(mgcp_ep_line_flags(&gw, 0) & PROTO_LF_CALL);
	fixture_stop();

	CASE("inbound: r1 to r5 ring the distinctive patterns, r0 the default");
	up(&fx_rfc);
	phf.ring_template = 0;
	ca_cmd("RQNT", EP1, "X: 11\r\nS: L/r4\r\n");
	CHECK_UINT(hop_last(PHF_RING_START)->val, 4);
	ca_cmd("RQNT", EP1, "X: 12\r\nS: L/r1\r\n");
	CHECK_UINT(hop_last(PHF_RING_START)->val, 1);
	ca_cmd("RQNT", EP1, "X: 13\r\nS: L/r7\r\n");
	CHECK_UINT(hop_last(PHF_RING_START)->val, 0);
	fixture_stop();

	CASE("inbound: rs is a short burst of the plain ring");
	up(&fx_rfc);
	ca_cmd("RQNT", EP1, "X: 14\r\nS: L/rs\r\n");
	CHECK_UINT(hop_last(PHF_RING_START)->duration_ms, 500);
	CHECK(!hop_last(PHF_RING_START)->cid_set);
	fixture_stop();

	CASE("inbound: ci rides the ring as its caller id");
	up(&fx_rfc);
	ca_cmd("RQNT", EP1, "X: 15\r\nS: L/rg, L/ci(09/14/17/26, \"555 1212\", "
	       "\"John Doe\")\r\n");
	CHECK(hop_last(PHF_RING_START)->cid_set);
	CHECK(!strcmp(hop_last(PHF_RING_START)->cid.number, "5551212"));
	CHECK(!strcmp(hop_last(PHF_RING_START)->cid.name, "John Doe"));
	CHECK(!strcmp(hop_last(PHF_RING_START)->cid.datetime, "09141726"));
	CHECK_UINT(hop_count(PHF_CID_SEND), 0);
	fixture_stop();

	CASE("inbound: a seize stops the ring and sends L/hd");
	up(&fx_rfc);
	ca_cmd("RQNT", EP1, "X: 16\r\nR: L/hd(N)\r\nS: L/rg\r\n");
	mark_set();
	ev_hook(0, HAL_HOOK_SEIZE);
	CHECK_UINT(hop_count(PHF_RING_STOP), 1);
	r = sent("NTFY");
	CHECK(has(r, "X: 16\r\n") && has(r, "O: L/hd\r\n"));
	fixture_stop();

	CASE("inbound: ci alone on hook sends the record");
	up(&fx_rfc);
	ca_cmd("RQNT", EP1, "X: 17\r\nS: L/ci(10/01/09/30,5551234,Bob)\r\n");
	CHECK_UINT(hop_count(PHF_CID_SEND), 1);
	CHECK_UINT(hop_last(PHF_CID_SEND)->val, HAL_CID_TYPE1);
	CHECK(!strcmp(hop_last(PHF_CID_SEND)->cid.name, "Bob"));
	fixture_stop();

	CASE("inbound: rg off hook gets 401, dl on hook 402");
	offhook_up(&fx_rfc);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 18\r\nS: L/rg\r\n")), 401);
	fixture_stop();
	up(&fx_rfc);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 19\r\nS: L/dl\r\n")), 402);
	CHECK_UINT(hop_count(PHF_TONE_PLAY), 0);
	fixture_stop();

	CASE("glare: hd asked off hook gets 401, hu and hf on hook 402");
	offhook_up(&fx_rfc);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 1A\r\nR: L/hd(N)\r\n")),
		   401);
	fixture_stop();
	up(&fx_rfc);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 1B\r\nR: L/hu(N)\r\n")),
		   402);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 1C\r\nR: L/hf(N)\r\n")),
		   402);
	fixture_stop();

	CASE("call waiting: wt1 with ci alerts with the caller");
	offhook_up(&fx_rfc);
	call_up(id, sizeof(id));
	r = ca_cmd("CRCX", EP1, "C: B2\r\nM: inactive\r\n\r\n" SDP_PCMA);
	CHECK_UINT(code_of(r), 200);
	conn_id_of(r, id2, sizeof(id2));
	r = ca_cmd("RQNT", EP1, "X: 20\r\nR: L/hf(N), L/hu(N)\r\n"
		   "S: L/wt1, L/ci(,5550000,Carol)\r\n");
	CHECK_UINT(code_of(r), 200);
	CHECK(hop_last(PHF_CW_ALERT)->on);
	CHECK_UINT(hop_last(PHF_CW_ALERT)->val, 1);
	CHECK(!strcmp(hop_last(PHF_CW_ALERT)->cid.name, "Carol"));
	mark_set();
	ev_hook(0, HAL_HOOK_FLASH);
	CHECK(!hop_last(PHF_CW_ALERT)->on && hop_count(PHF_CW_ALERT) == 1);
	CHECK(has(sent("NTFY"), "O: L/hf\r\n"));
	fixture_stop();

	CASE("call waiting: wt and wt4 map to variants 0 and 4");
	offhook_up(&fx_rfc);
	ca_cmd("RQNT", EP1, "X: 21\r\nS: L/wt\r\n");
	CHECK_UINT(hop_count(PHF_CW_ALERT), 1);
	CHECK(hop_last(PHF_CW_ALERT)->on);
	CHECK_UINT(hop_last(PHF_CW_ALERT)->val, 0);
	ca_cmd("RQNT", EP1, "X: 22\r\nS: L/wt4\r\n");
	CHECK_UINT(hop_count(PHF_CW_ALERT), 3);
	CHECK(hop_last(PHF_CW_ALERT)->on);
	CHECK_UINT(hop_last(PHF_CW_ALERT)->val, 4);
	fixture_stop();

	CASE("call waiting: the two swaps by MDCX modes");
	offhook_up(&fx_rfc);
	call_up(id, sizeof(id));
	r = ca_cmd("CRCX", EP1, "C: B2\r\nM: inactive\r\n\r\n" SDP_PCMA);
	conn_id_of(r, id2, sizeof(id2));
	mark_set();
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\nM: inactive\r\n", id);
	CHECK_UINT(code_of(ca_cmd("MDCX", EP1, rest)), 200);
	snprintf(rest, sizeof(rest), "C: B2\r\nI: %s\r\nM: sendrecv\r\n", id2);
	CHECK_UINT(code_of(ca_cmd("MDCX", EP1, rest)), 200);
	CHECK_UINT(hop_last(PHF_MEDIA_CONFIG_SET)->stream, 1);
	CHECK_UINT(hop_last(PHF_MEDIA_CONFIG_SET)->media.direction,
		   HAL_MEDIA_SENDRECV);
	CHECK_UINT(hop_count(PHF_MEDIA_START), 1);
	snprintf(rest, sizeof(rest), "C: B2\r\nI: %s\r\nM: inactive\r\n", id2);
	CHECK_UINT(code_of(ca_cmd("MDCX", EP1, rest)), 200);
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\nM: sendrecv\r\n", id);
	CHECK_UINT(code_of(ca_cmd("MDCX", EP1, rest)), 200);
	CHECK_UINT(hop_last(PHF_MEDIA_CONFIG_SET)->stream, 0);
	CHECK_UINT(hop_last(PHF_MEDIA_CONFIG_SET)->media.direction,
		   HAL_MEDIA_SENDRECV);
	fixture_stop();

	CASE("mwi: vmwi turns the indication on once and vmwi(-) off");
	up(&fx_rfc);
	ca_cmd("RQNT", EP1, "X: 30\r\nS: L/vmwi\r\n");
	CHECK_UINT(hop_count(PHF_MWI_SET), 1);
	CHECK(hop_last(PHF_MWI_SET)->on);
	ca_cmd("RQNT", EP1, "X: 31\r\nS: L/vmwi(+)\r\n");
	ca_cmd("RQNT", EP1, "X: 32\r\n");
	CHECK_UINT(hop_count(PHF_MWI_SET), 1);
	ca_cmd("RQNT", EP1, "X: 33\r\nS: L/vmwi(-)\r\n");
	CHECK_UINT(hop_count(PHF_MWI_SET), 2);
	CHECK(!hop_last(PHF_MWI_SET)->on);
	fixture_stop();

	CASE("mwi: a restart leaves vmwi off and sends no record");
	up(&fx_rfc);
	ca_cmd("RQNT", EP1, "X: 34\r\nS: L/vmwi\r\n");
	mgcp_ep_service(&gw, 0, false, MGCP_EP_REASON_OOS);
	mgcp_ep_service(&gw, 0, true, 0);
	CHECK_UINT(hop_count(PHF_MWI_SET), 1);
	CHECK(!gw.ep[0].vmwi_on);
	fixture_stop();

	CASE("signals: an oc and an of reach the call agent as asked");
	up(&fx_rfc);
	ca_cmd("RQNT", EP1, "X: 40\r\nR: L/oc(N), L/hd(N)\r\n"
	       "S: L/rg(to=6000)\r\n");
	mark_set();
	fake_advance(6000);
	CHECK_UINT(hop_count(PHF_RING_STOP), 1);
	CHECK(has(sent("NTFY"), "O: L/oc(L/rg)\r\n"));
	fixture_stop();
	offhook_up(&fx_rfc);
	ca_cmd("RQNT", EP1, "X: 41\r\nR: G/of(N)\r\nS: G/rt\r\n");
	mark_set();
	mgcp_ep_signal_result(&gw, 0, PROTO_SIG_TONE, false);
	CHECK(has(sent("NTFY"), "O: G/of(G/rt)\r\n"));
	fixture_stop();

	CASE("signals: a ring the backend fails is an of");
	up(&fx_rfc);
	ca_cmd("RQNT", EP1, "X: 42\r\nR: L/of(N), L/hd(N)\r\nS: L/rg\r\n");
	mark_set();
	mgcp_ep_ring_failed(&gw, 0);
	CHECK(has(sent("NTFY"), "O: L/of(L/rg)\r\n"));
	fixture_stop();

	CASE("signals: osi removes the battery for 900 ms");
	up(&fx_rfc);
	ca_cmd("RQNT", EP1, "X: 43\r\nS: L/osi\r\n");
	CHECK_UINT(hop_last(PHF_BATTERY_SET)->val, PROTO_BATT_DOWN);
	fake_advance(900);
	CHECK_UINT(hop_last(PHF_BATTERY_SET)->val, PROTO_BATT_ACTIVE);
	fixture_stop();

	CASE("signals: lsa reverses the battery, 513 without polarity");
	up(&fx_rfc);
	ca_cmd("RQNT", EP1, "X: 44\r\nS: L/lsa\r\n");
	CHECK_UINT(hop_last(PHF_BATTERY_SET)->val, PROTO_BATT_REVERSE);
	ca_cmd("RQNT", EP1, "X: 45\r\nS: L/lsa(-)\r\n");
	CHECK_UINT(hop_last(PHF_BATTERY_SET)->val, PROTO_BATT_ACTIVE);
	phf.hal_features &= ~(uint32_t)HAL_FEAT_POLARITY;
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 46\r\nS: L/lsa\r\n")),
		   513);
	fixture_stop();

	CASE("signals: digits queue, a new request cancels the pending ones");
	offhook_up(&fx_rfc);
	ca_cmd("RQNT", EP1, "X: 47\r\nS: D/1, D/2, D/3\r\n");
	CHECK_UINT(hop_count(PHF_DIGIT_PLAY), 3);
	CHECK(has(ca_cmd("AUEP", EP1, "F: S\r\n"), "S: D/2,D/3\r\n"));
	mark_set();
	ca_cmd("RQNT", EP1, "X: 48\r\n");
	CHECK_UINT(hop_count(PHF_DIGIT_PLAY_CANCEL), 1);
	fixture_stop();

	CASE("signals: codes 513, 522, 518 and 538");
	offhook_up(&fx_rfc);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 50\r\nS: L/ht\r\n")), 513);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 51\r\nS: L/zz\r\n")), 522);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 52\r\nS: Q/zz\r\n")), 518);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 53\r\nS: L/bz(to=x)\r\n")),
		   538);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 54\r\nS: L/ci(1,2)\r\n")),
		   538);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 55\r\nS: G/rt@ABC\r\n")),
		   515);
	fixture_stop();

	CASE("events: codes 523, 518, 512 and 508");
	offhook_up(&fx_rfc);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 60\r\nR: L/hu(S)\r\n")),
		   523);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 61\r\nR: L/hu(N,A)\r\n")),
		   523);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 62\r\nR: L/hu(Z)\r\n")),
		   518);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 63\r\nR: G/ld\r\n")), 512);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 64\r\nQ: maybe\r\n")),
		   508);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "R: L/hu\r\n")), 510);
	fixture_stop();

	CASE("digit map: a refused map gets 537 and keeps the old one");
	offhook_up(&fx_rfc);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 70\r\nD: (9Q)\r\n")), 537);
	CHECK(!strcmp(gw.ep[0].dmap, "x.T"));
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 71\r\nD: (9T11)\r\n")),
		   200);
	fixture_stop();

	CASE("digit map: ncs refuses a mid-map T with 510");
	offhook_up(&fx_ncs);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 72\r\nD: (9T11)\r\n")),
		   510);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 73\r\nD: (911T)\r\n")),
		   200);
	fixture_stop();

	CASE("embedded: E runs its request when the event fires");
	up(&fx_rfc);
	ca_cmd("RQNT", EP1, "X: 74\r\nR: L/hd(E(R(L/hu(N),D/[0-9](D)),"
	       "S(L/dl)))\r\n");
	mark_set();
	ev_hook(0, HAL_HOOK_SEIZE);
	CHECK_UINT(sent_count("NTFY"), 0);
	CHECK_UINT(hop_last(PHF_TONE_PLAY)->val, HAL_TONE_DIAL);
	ev_hook(0, HAL_HOOK_RELEASE);
	CHECK(has(sent("NTFY"), "O: L/hu\r\n"));
	fixture_stop();
}

/* --------------------------------------------------------------- fax */

static void fax_tests(void)
{
	char id[MGCP_ID_MAX];

	CASE("fax: a fax tone row reports ft once and applies passthrough");
	offhook_up(&fx_loop);
	call_up(id, sizeof(id));
	ca_cmd("RQNT", EP1, "X: 80\r\nR: G/ft(N), G/mt(N)\r\n");
	mark_set();
	ev_tone(0, HAL_TONE_DET_V21_PREAMBLE, false);
	CHECK_UINT(hop_count(PHF_MEDIA_FAX_APPLY), 1);
	CHECK_UINT(hop_last(PHF_MEDIA_FAX_APPLY)->fax.mode,
		   HAL_FAX_PASSTHROUGH);
	CHECK_UINT(hop_last(PHF_MEDIA_FAX_APPLY)->fax.codec, HAL_CODEC_PCMU);
	CHECK_UINT(sent_count("NTFY"), 1);
	CHECK(has(sent("NTFY"), "O: G/ft\r\n"));
	ca_answer(sent("NTFY"), 200, NULL);
	ev_tone(0, HAL_TONE_DET_V21_PREAMBLE, true);
	CHECK_UINT(sent_count("NTFY"), 1);
	ev_tone(0, HAL_TONE_DET_V21_PREAMBLE, false);
	CHECK_UINT(sent_count("NTFY"), 2);
	CHECK_UINT(hop_count(PHF_MEDIA_FAX_APPLY), 1);
	fixture_stop();

	CASE("fax: an answer tone reports mt");
	offhook_up(&fx_rfc);
	call_up(id, sizeof(id));
	ca_cmd("RQNT", EP1, "X: 81\r\nR: G/mt(N)\r\n");
	mark_set();
	ev_tone(0, HAL_TONE_DET_ANS, false);
	CHECK(has(sent("NTFY"), "O: G/mt\r\n"));
	fixture_stop();

	CASE("fax: ncs reports ft in the line package");
	offhook_up(&fx_ncs);
	ca_cmd("RQNT", EP1, "X: 82\r\nR: L/ft(N)\r\n");
	mark_set();
	ev_tone(0, HAL_TONE_DET_CNG, false);
	CHECK(has(sent("NTFY"), "O: L/ft\r\n"));
	fixture_stop();
}

/* ------------------------------------------------------------ quarantine */

static void quarantine_tests(void)
{
	const char *n1;
	unsigned int i;

	CASE("quarantine: step holds events until the next request");
	offhook_up(&fx_rfc);
	ca_cmd("RQNT", EP1, "X: 90\r\nR: L/hf(N), L/hu(N)\r\n");
	mark_set();
	ev_hook(0, HAL_HOOK_FLASH);
	n1 = sent("NTFY");
	ev_hook(0, HAL_HOOK_FLASH);
	CHECK_UINT(sent_count("NTFY"), 1);
	ca_answer(n1, 200, NULL);
	CHECK_UINT(sent_count("NTFY"), 1);
	CHECK_UINT(gw.ep[0].nquar, 1);
	ca_cmd("RQNT", EP1, "X: 91\r\nR: L/hf(N), L/hu(N)\r\n");
	CHECK_UINT(sent_count("NTFY"), 2);
	fixture_stop();

	CASE("quarantine: loop processes the buffer when the NTFY is answered");
	offhook_up(&fx_rfc);
	ca_cmd("RQNT", EP1, "X: 92\r\nR: L/hf(N), L/hu(N)\r\nQ: loop\r\n");
	mark_set();
	ev_hook(0, HAL_HOOK_FLASH);
	n1 = sent("NTFY");
	ev_hook(0, HAL_HOOK_FLASH);
	CHECK_UINT(sent_count("NTFY"), 1);
	ca_answer(n1, 200, NULL);
	CHECK_UINT(sent_count("NTFY"), 2);
	fixture_stop();

	CASE("quarantine: discard drops the held events");
	offhook_up(&fx_rfc);
	ca_cmd("RQNT", EP1, "X: 93\r\nR: L/hf(N), L/hu(N)\r\n");
	mark_set();
	ev_hook(0, HAL_HOOK_FLASH);
	ca_answer(sent("NTFY"), 200, NULL);
	ev_hook(0, HAL_HOOK_FLASH);
	ca_cmd("RQNT", EP1, "X: 94\r\nR: L/hf(N), L/hu(N)\r\nQ: discard\r\n");
	CHECK_UINT(sent_count("NTFY"), 1);
	CHECK_UINT(gw.ep[0].nquar, 0);
	fixture_stop();

	CASE("quarantine: a full buffer discards the excess, not the oldest");
	offhook_up(&fx_loop);
	ca_cmd("RQNT", EP1, "X: 95\r\nR: D/[0-9*#A-D](N)\r\n");
	mark_set();
	ev_digit(0, '0');
	n1 = sent("NTFY");
	for (i = 0; i < MGCP_QUAR_MAX + 2; i++)
		ev_digit(0, "123456789*#ABCD0123"[i]);
	CHECK_UINT(gw.ep[0].nquar, MGCP_QUAR_MAX);
	CHECK_UINT(gw.ep[0].quar_dropped, 2);
	mark_set();
	ca_answer(n1, 200, NULL);
	CHECK(has(sent("NTFY"), "O: D/1\r\n"));
	fixture_stop();

	CASE("quarantine: lockstep, a request waits for the NTFY answer");
	offhook_up(&fx_loop);
	ca_cmd("RQNT", EP1, "X: 96\r\nR: L/hf(N), L/hu(N)\r\nQ: loop\r\n");
	mark_set();
	ev_hook(0, HAL_HOOK_FLASH);
	n1 = sent("NTFY");
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 97\r\nR: L/hf(N), "
				  "L/hu(N)\r\nQ: loop\r\n")), 200);
	ev_hook(0, HAL_HOOK_FLASH);
	CHECK_UINT(sent_count("NTFY"), 1);
	ca_answer(n1, 200, NULL);
	CHECK_UINT(sent_count("NTFY"), 2);
	fixture_stop();

	CASE("persistent: a requested action replaces the default Notify");
	up(&fx_rfc);
	ca_cmd("RQNT", EP1, "X: 98\r\nR: L/hd(I)\r\n");
	ev_hook(0, HAL_HOOK_SEIZE);
	CHECK_UINT(sent_count("NTFY"), 0);
	fixture_stop();
}

/* ------------------------------------------------- audits, codes, service */

static void audit_tests(void)
{
	char id[MGCP_ID_MAX], rest[256];
	const char *r;

	CASE("auep: F: answers the endpoint state");
	offhook_up(&fx_rfc);
	call_up(id, sizeof(id));
	ca_cmd("RQNT", EP1, "X: A0\r\nR: L/hu(N)\r\nD: (xxxx)\r\n");
	r = ca_cmd("AUEP", EP1, "F: X,N,R,D,Q,ES,I,RM,E,PL,MD,B,ZZ\r\n");
	CHECK(!strncmp(r, "200", 3));
	CHECK(has(r, "X: A0\r\n") && has(r, "N: ca@192.0.2.1:2727\r\n"));
	CHECK(has(r, "R: L/hu(N),L/hd,L/hf\r\n"));
	CHECK(has(r, "D: xxxx\r\n") && has(r, "Q: process,step\r\n"));
	CHECK(has(r, "ES: L/hd\r\n") && has(r, "RM: restart\r\n"));
	CHECK(has(r, "E: 000\r\n") && has(r, "PL: L:1,G:1,D:1\r\n"));
	CHECK(has(r, "MD: 4096\r\n") && has(r, "\r\nB:\r\n"));
	snprintf(rest, sizeof(rest), "I: %s\r\n", id);
	CHECK(has(r, rest));
	fixture_stop();

	CASE("auep: A: lists the codecs and the modes");
	up(&fx_rfc);
	r = ca_cmd("AUEP", EP1, "F: A\r\n");
	CHECK(has(r, "A: a:PCMU;PCMA;G729;image/t38, p:10-30"));
	CHECK(has(r, "v:L;G;D, m:sendonly;recvonly;sendrecv;inactive\r\n"));
	fixture_stop();

	CASE("auep: the all-of wildcard lists the endpoints");
	up(&fx_rfc);
	r = ca_cmd("AUEP", "*@gw.example.net", NULL);
	CHECK(has(r, "Z: " EP1 "\r\n") && has(r, "Z: " EP2 "\r\n"));
	fixture_stop();

	CASE("aucx: the descriptors and the mode of a connection");
	offhook_up(&fx_rfc);
	call_up(id, sizeof(id));
	snprintf(rest, sizeof(rest), "I: %s\r\nF: C,M,LC,RC\r\n", id);
	r = ca_cmd("AUCX", EP1, rest);
	CHECK(has(r, "C: A1\r\n") && has(r, "M: sendrecv\r\n"));
	CHECK(has(r, "m=audio 16384") && has(r, "m=audio 30000"));
	CHECK(strstr(r, "m=audio 16384") < strstr(r, "m=audio 30000"));
	fixture_stop();

	CASE("epcf: e:mu is kept, another value gets 532");
	up(&fx_rfc);
	CHECK_UINT(code_of(ca_cmd("EPCF", EP1, "B: e:mu\r\n")), 200);
	CHECK(!strcmp(gw.ep[0].bearer, "mu"));
	CHECK_UINT(code_of(ca_cmd("EPCF", EP1, "B: e:x\r\n")), 532);
	fixture_stop();
	up(&fx_ncs);
	CHECK_UINT(code_of(ca_cmd("EPCF", EP1, "B: e:mu\r\n")), 510);
	fixture_stop();

	CASE("codes: 500 for an unknown endpoint or domain, 528, 504");
	up(&fx_rfc);
	CHECK_UINT(code_of(ca_cmd("RQNT", "aaln/9@gw.example.net",
				  "X: 1\r\n")), 500);
	CHECK_UINT(code_of(ca_cmd("RQNT", "aaln/1@other.example.net",
				  "X: 1\r\n")), 500);
	{
		char buf[128];
		int n = snprintf(buf, sizeof(buf), "RQNT 7001 " EP1
				 " MGCP 2.0\r\nX: 1\r\n");
		unsigned int before = fk.sends;

		mgcp_txn_recv(&mgr, buf, (size_t)n,
			      (struct sockaddr *)&ca_sa, ca_len);
		CHECK(fk.sends == before + 1 &&
		      !strncmp(fk.send[before].buf, "528 7001", 8));
	}
	CHECK_UINT(code_of(ca_cmd("NTFY", EP1, "X: 1\r\nO: L/hd\r\n")), 504);
	fixture_stop();
	up(&fx_ncs);
	CHECK_UINT(code_of(ca_cmd("NTFY", EP1, "X: 1\r\nO: L/hd\r\n")), 510);
	fixture_stop();

	CASE("codes: ncs answers the codes the two dialects share");
	offhook_up(&fx_ncs);
	call_up(id, sizeof(id));
	CHECK_UINT(code_of(ca_cmd("RQNT", "aaln/9@gw.example.net",
				  "X: 1\r\n")), 500);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 1\r\nS: G/rt\r\n")), 518);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 1\r\nR: L/ld\r\n")), 512);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 1\r\nS: L/zz\r\n")), 522);
	snprintf(rest, sizeof(rest), "X: 1\r\nS: L/rt@%s\r\n", id);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, rest)), 513);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 1\r\nR: L/hu(S)\r\n")),
		   523);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 1\r\nQ: maybe\r\n")),
		   508);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 1\r\nR: L/hd(N)\r\n")),
		   401);
	CHECK_UINT(code_of(ca_cmd("CRCX", EP1, "C: B2\r\nM: loopback\r\n")),
		   517);
	CHECK_UINT(code_of(ca_cmd("CRCX", EP1, "C: B2\r\nM: sendrecv\r\n")),
		   527);
	CHECK_UINT(code_of(ca_cmd("CRCX", EP1, "C: B2\r\nM: recvonly\r\n"
				  "L: a:G723\r\n")), 534);
	CHECK_UINT(code_of(ca_cmd("CRCX", EP1, "C: B2\r\nM: recvonly\r\n"
				  "L: e:maybe\r\n")), 532);
	CHECK_UINT(code_of(ca_cmd("MDCX", EP1, "C: A1\r\nI: 9\r\n")), 515);
	snprintf(rest, sizeof(rest), "C: B2\r\nI: %s\r\n", id);
	CHECK_UINT(code_of(ca_cmd("MDCX", EP1, rest)), 516);
	mgcp_ep_service(&gw, 0, false, MGCP_EP_REASON_OOS);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 1\r\n")), 501);
	fixture_stop();
}

static void service_tests(void)
{
	char id[MGCP_ID_MAX];
	const char *r;

	CASE("service: out sends DLCX with E: per connection, then RSIP");
	offhook_up(&fx_rfc);
	call_up(id, sizeof(id));
	mgcp_ep_service(&gw, 0, false, MGCP_EP_REASON_OOS);
	r = sent("DLCX");
	CHECK(has(r, "C: A1\r\n") && has(r, "E: 901\r\n"));
	CHECK(has(r, "I: ") && has(r, id));
	r = sent("RSIP");
	CHECK(has(r, " " EP1 " ") && has(r, "RM: forced\r\n"));
	CHECK(fk.sends >= 2 && !strncmp(fk.send[fk.sends - 1].buf, "RSIP", 4));
	CHECK(!gw.ep[0].conn[0].used);
	CHECK_UINT(hop_count(PHF_MEDIA_STOP), 1);
	fixture_stop();

	CASE("service: a non-audit command out of service gets 501");
	up(&fx_rfc);
	mgcp_ep_service(&gw, 0, false, MGCP_EP_REASON_FAULT);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 1\r\n")), 501);
	r = ca_cmd("AUEP", EP1, "F: RM,E\r\n");
	CHECK(has(r, "RM: forced\r\n") && has(r, "E: 900\r\n"));
	CHECK(!strcmp(mgcp_ep_state_name(&gw, 0), "out_of_service"));
	fixture_stop();

	CASE("service: back in service sends RSIP restart for the endpoint");
	up(&fx_rfc);
	mgcp_ep_service(&gw, 0, false, MGCP_EP_REASON_OOS);
	mark_set();
	mgcp_ep_service(&gw, 0, true, 0);
	r = sent("RSIP");
	CHECK(has(r, " " EP1 " ") && has(r, "RM: restart\r\n"));
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 1\r\n")), 405);
	ca_answer(r, 200, NULL);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 2\r\n")), 200);
	fixture_stop();
}

static void disc_tests(void)
{
	char id[MGCP_ID_MAX];
	const char *r, *rsip;

	CASE("disconnected: a NTFY that gets no answer starts the procedure");
	offhook_up(&fx_rfc);
	call_up(id, sizeof(id));
	ca_cmd("RQNT", EP1, "X: B0\r\nR: L/hf(N), L/hu(N)\r\n");
	ev_hook(0, HAL_HOOK_FLASH);
	mark_set();
	fake_advance(30000);
	CHECK(mgcp_txn_disc_active(&mgr));
	fake_advance(15000);
	rsip = sent("RSIP");
	CHECK(has(rsip, "RSIP ") && has(rsip, " " EP1 " ") &&
	      has(rsip, "RM: disconnected\r\n") && has(rsip, "RD: "));
	CHECK_UINT(hop_count(PHF_MEDIA_STOP), 0);
	CHECK(gw.ep[0].conn[0].used);
	fixture_stop();

	CASE("disconnected: a command gets its answer behind the RSIP");
	offhook_up(&fx_rfc);
	ca_cmd("RQNT", EP1, "X: B1\r\nR: L/hf(N), L/hu(N)\r\n");
	ev_hook(0, HAL_HOOK_FLASH);
	fake_advance(30000);
	CHECK(mgcp_txn_disc_active(&mgr));
	mark_set();
	r = ca_cmd("RQNT", EP1, "X: B2\r\nR: L/hf(N), L/hu(N)\r\n");
	CHECK(!strncmp(r, "RSIP ", 5) && has(r, "RM: disconnected\r\n"));
	CHECK(has(r, "\r\n.\r\n200 "));
	/* the same transaction goes to the notified entity on its own */
	CHECK_UINT(sent_count("RSIP"), 2);
	rsip = NULL;
	{
		unsigned int i;

		for (i = mark; i < fk.sends; i++) {
			if (!strncmp(fk.send[i].buf, "RSIP", 4) &&
			    !has(fk.send[i].buf, "\r\n.\r\n"))
				rsip = fk.send[i].buf;
		}
	}
	CHECK(rsip && txid_of(rsip) == txid_of(r));
	ca_answer(rsip ? rsip : "", 200, NULL);
	CHECK(!mgcp_txn_disc_active(&mgr));
	CHECK(!gw.ep[0].notif);
	fixture_stop();

	CASE("disconnected: an audit gets no RSIP in front");
	offhook_up(&fx_rfc);
	ca_cmd("RQNT", EP1, "X: B3\r\nR: L/hf(N), L/hu(N)\r\n");
	ev_hook(0, HAL_HOOK_FLASH);
	fake_advance(30000);
	r = ca_cmd("AUEP", EP1, "F: RM\r\n");
	CHECK(!strncmp(r, "200", 3) && has(r, "RM: disconnected\r\n"));
	fixture_stop();
}

/* --------------------------------------------- RSIP, entities, recovery */

static void rsip_tests(void)
{
	const struct fake_send *rec;
	const char *r, *rsip;

	CASE("restart: an RSIP that does not go out is sent on a command");
	fixture_start(&fx_rfc);
	fk.send_fail = true;
	mgcp_ep_start(&gw);
	fake_advance(10001);
	fk.send_fail = false;
	CHECK_UINT(fk.sends, 0);
	r = ca_cmd("RQNT", EP1, "X: 1\r\n");
	CHECK_UINT(code_of(r), 405);
	CHECK_UINT(sent_count("RSIP"), 1);
	fixture_stop();

	CASE("restart: an RSIP that does not go out is sent after the delay");
	fixture_start(&fx_rfc);
	fk.send_fail = true;
	mgcp_ep_start(&gw);
	/* the fake entropy draws 9256 ms each time */
	fake_advance(9256);
	fk.send_fail = false;
	CHECK_UINT(fk.sends, 0);
	fake_advance(10001);
	CHECK(sent_count("RSIP") >= 1);
	CHECK(has(sent("RSIP"), "RM: restart"));
	CHECK(sent_rec("RSIP") && sent_rec("RSIP")->at_ms >= 9256 + 1000 &&
	      sent_rec("RSIP")->at_ms <= 9256 + 10000);
	fixture_stop();
	/* a zero draw waits the 1 s minimum */
	fixture_start(&fx_rfc);
	phf.accept = false;
	fk.send_fail = true;
	mgcp_ep_start(&gw);
	fake_advance(999);
	fk.send_fail = false;
	CHECK_UINT(fk.sends, 0);
	fake_advance(1);
	CHECK(sent_rec("RSIP") && sent_rec("RSIP")->at_ms == 1000);
	fixture_stop();

	CASE("restart: a 4xx to the RSIP sends it again");
	fixture_start(&fx_rfc);
	mgcp_ep_start(&gw);
	fake_advance(10001);
	r = sent("RSIP");
	mark_set();
	ca_answer(r, 400, NULL);
	CHECK_UINT(sent_count("RSIP"), 1);
	CHECK(txid_of(sent("RSIP")) != txid_of(r));
	fixture_stop();

	CASE("restarting: ncs puts the RSIP in front of an audit response");
	fixture_start(&fx_ncs);
	mgcp_ep_start(&gw);
	r = ca_cmd("AUEP", EP1, "F: RM\r\n");
	CHECK(!strncmp(r, "RSIP ", 5) && has(r, "RM: restart"));
	CHECK(has(r, "\r\n.\r\n200 "));
	fixture_stop();

	CASE("service: ncs puts the endpoint's own RSIP in front of 520");
	up(&fx_ncs);
	mgcp_ep_service(&gw, 0, false, MGCP_EP_REASON_OOS);
	mgcp_ep_service(&gw, 0, true, 0);
	r = ca_cmd("RQNT", EP1, "X: 1\r\n");
	CHECK(!strncmp(r, "RSIP ", 5) && has(r, " " EP1 " "));
	CHECK(has(r, "\r\n.\r\n520 "));
	fixture_stop();

	CASE("disconnected: an RSIP with no address arms the next attempt");
	offhook_up(&fx_rfc);
	ca_cmd("RQNT", EP1, "X: B4\r\nR: L/hf(N), L/hu(N)\r\n");
	ev_hook(0, HAL_HOOK_FLASH);
	fk.dst_fail = true;
	mark_set();
	fake_advance(30000);
	CHECK(mgcp_txn_disc_active(&mgr));
	fake_advance(15000);
	CHECK_UINT(sent_count("RSIP"), 0);
	/* each attempt that found no address doubles the delay (4.4.7) */
	CHECK(mgr.disc.td_ms > 1000);
	CHECK(fake_timer_due(&mgr.disc.timer) != 0);
	fk.dst_fail = false;
	fake_advance(mgr.disc.td_ms + 1);
	CHECK(sent_count("RSIP") >= 1);
	fixture_stop();

	CASE("disconnected: each notified entity gets its own RSIP");
	offhook_up(&fx_rfc);
	ca_cmd("RQNT", EP1, "X: C0\r\nR: L/hf(N), L/hu(N)\r\n");
	ca_cmd("RQNT", EP2, "X: C1\r\nN: ca2@[192.0.2.2]:2727\r\n"
	       "R: L/hd(N)\r\n");
	ev_hook(0, HAL_HOOK_FLASH);
	ev_hook(1, HAL_HOOK_SEIZE);
	CHECK(sent_to("NTFY", "192.0.2.2") != NULL);
	fake_advance(30000);
	CHECK(mgcp_txn_disc_active(&mgr));
	mark_set();
	fake_advance(15000);
	rec = sent_to("RSIP", "192.0.2.2");
	CHECK(rec && has(rec->buf, " " EP2 " ") &&
	      has(rec->buf, "RM: disconnected\r\n"));
	rec = sent_to("RSIP", "192.0.2.1");
	CHECK(rec && has(rec->buf, " " EP1 " "));
	ca_answer(rec ? rec->buf : "", 200, NULL);
	CHECK(has(ca_cmd("AUEP", EP1, "F: RM\r\n"), "RM: restart\r\n"));
	CHECK(has(ca_cmd("AUEP", EP2, "F: RM\r\n"), "RM: disconnected\r\n"));
	CHECK(mgcp_txn_disc_active(&mgr));
	fixture_stop();

	CASE("disconnected: a 200 with N: moves the entity before the drain");
	offhook_up(&fx_rfc);
	ca_cmd("RQNT", EP1, "X: B5\r\nR: L/hf(N), L/hu(N)\r\nQ: loop\r\n");
	ev_hook(0, HAL_HOOK_FLASH);
	fake_advance(30000);
	ev_hook(0, HAL_HOOK_FLASH);
	mark_set();
	fake_advance(15000);
	rec = sent_to("RSIP", NULL);
	rsip = rec ? rec->buf : "";
	CHECK(has(rsip, "RSIP "));
	mark_set();
	ca_answer(rsip, 200, "N: ca2@[192.0.2.2]:2727\r\n");
	rec = sent_rec("NTFY");
	CHECK(rec && !strcmp(rec->host, "192.0.2.2"));
	CHECK(rec && has(rec->buf, "O: L/hf\r\n"));
	fixture_stop();

	CASE("disconnected: the end sends the event held in the quarantine");
	offhook_up(&fx_rfc);
	ca_cmd("RQNT", EP1, "X: B6\r\nR: L/hf(N), L/hu(N)\r\nQ: loop\r\n");
	ev_hook(0, HAL_HOOK_FLASH);
	fake_advance(30000);
	ev_hook(0, HAL_HOOK_FLASH);
	CHECK_UINT(gw.ep[0].nquar, 1);
	mark_set();
	fake_advance(15000);
	rec = sent_to("RSIP", NULL);
	rsip = rec ? rec->buf : "";
	CHECK_UINT(sent_count("NTFY"), 0);
	mark_set();
	ca_answer(rsip, 200, NULL);
	CHECK(has(sent("NTFY"), "O: L/hf\r\n"));
	CHECK(!gw.ep[0].notif || gw.ep[0].ntfy_txid);
	fixture_stop();
}

/* --------------------------------------------- command and request checks */

static void check_tests(void)
{
	char id[MGCP_ID_MAX], rest[1024];
	const char *r;

	CASE("codes: an unknown X+ parameter gets 511 and changes nothing");
	up(&fx_rfc);
	ca_cmd("RQNT", EP1, "X: 1\r\n");
	r = ca_cmd("RQNT", EP1, "X: 2\r\nX+vendor: 1\r\nS: L/vmwi\r\n");
	CHECK_UINT(code_of(r), 511);
	CHECK(!strcmp(gw.ep[0].rid, "1"));
	CHECK_UINT(hop_count(PHF_MWI_SET), 0);
	fixture_stop();

	CASE("mdcx and dlcx: a missing, empty or bad C: gets 510");
	offhook_up(&fx_rfc);
	call_up(id, sizeof(id));
	snprintf(rest, sizeof(rest), "I: %s\r\nM: sendonly\r\n", id);
	CHECK_UINT(code_of(ca_cmd("MDCX", EP1, rest)), 510);
	snprintf(rest, sizeof(rest), "C:\r\nI: %s\r\nM: sendonly\r\n", id);
	CHECK_UINT(code_of(ca_cmd("MDCX", EP1, rest)), 510);
	snprintf(rest, sizeof(rest), "C: XYZ\r\nI: %s\r\nM: sendonly\r\n", id);
	CHECK_UINT(code_of(ca_cmd("MDCX", EP1, rest)), 510);
	snprintf(rest, sizeof(rest), "I: %s\r\n", id);
	CHECK_UINT(code_of(ca_cmd("DLCX", EP1, rest)), 510);
	CHECK(gw.ep[0].conn[0].used);
	CHECK_UINT(gw.ep[0].conn[0].mode, MGCP_MODE_SENDRECV);
	snprintf(rest, sizeof(rest), "I: %s\r\nF: M\r\n", id);
	CHECK_UINT(code_of(ca_cmd("AUCX", EP1, rest)), 200);
	fixture_stop();

	CASE("mdcx: a request parameter without X: gets 510");
	offhook_up(&fx_rfc);
	call_up(id, sizeof(id));
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\nM: sendonly\r\n"
		 "S: L/dl\r\n", id);
	CHECK_UINT(code_of(ca_cmd("MDCX", EP1, rest)), 510);
	CHECK_UINT(hop_count(PHF_TONE_PLAY), 0);
	CHECK_UINT(gw.ep[0].conn[0].mode, MGCP_MODE_SENDRECV);
	fixture_stop();

	CASE("dlcx: several connections with a request get 510");
	offhook_up(&fx_rfc);
	ca_cmd("CRCX", EP1, "C: A1\r\nM: inactive\r\n");
	ca_cmd("RQNT", EP1, "X: E0\r\n");
	r = ca_cmd("DLCX", EP1, "X: E1\r\nR: L/hu(N)\r\n");
	CHECK_UINT(code_of(r), 510);
	CHECK(!strcmp(gw.ep[0].rid, "E0"));
	CHECK(gw.ep[0].conn[0].used);
	fixture_stop();

	CASE("any of: a prefix that names no endpoint gets 500");
	up(&fx_rfc);
	CHECK_UINT(code_of(ca_cmd("CRCX", "foo/$@gw.example.net",
				  "C: A1\r\nM: inactive\r\n")), 500);
	fixture_stop();

	CASE("any of: a busy endpoint is skipped");
	up(&fx_rfc);
	ca_cmd("CRCX", EP1, "C: A1\r\nM: inactive\r\n");
	r = ca_cmd("CRCX", "aaln/$@gw.example.net", "C: A2\r\nM: inactive\r\n");
	CHECK_UINT(code_of(r), 200);
	CHECK(has(r, "Z: " EP2 "\r\n"));
	fixture_stop();

	CASE("events: a connection qualifier gets 512, a parameter 538");
	offhook_up(&fx_rfc);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 1\r\nR: D/5@DEAD\r\n")),
		   512);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1,
				  "X: 2\r\nR: D/5(N)(unknown=1)\r\n")), 538);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1,
				  "X: 3\r\nT: D/5(unknown=1)\r\n")), 538);
	CHECK(!strcmp(gw.ep[0].rid, "0"));
	fixture_stop();

	CASE("events: L/all is no glare, on hook or off hook");
	up(&fx_rfc);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 1\r\nR: L/all(N)\r\n")),
		   200);
	fixture_stop();
	offhook_up(&fx_rfc);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 1\r\nR: L/all(N)\r\n")),
		   200);
	fixture_stop();

	CASE("events: T: L/all under ncs arms the digits and the tones");
	offhook_up(&fx_ncs);
	r = ca_cmd("RQNT", EP1, "X: 1\r\nT: L/all\r\n");
	CHECK_UINT(code_of(r), 200);
	CHECK(hop_last(PHF_DETECTOR_ARM)->val & HAL_DETECT_DTMF);
	CHECK(hop_last(PHF_DETECTOR_ARM)->val & HAL_DETECT_MODEM);
	fixture_stop();

	CASE("events: N with E needs Q: loop");
	offhook_up(&fx_rfc);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1,
				  "X: 1\r\nR: L/hf(N,E(S(L/dl)))\r\n")), 523);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 2\r\nQ: loop\r\n"
				  "R: L/hf(N,E(S(L/dl)))\r\n")), 200);
	fixture_stop();

	CASE("digit map: a reversed range gets 522");
	offhook_up(&fx_rfc);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 1\r\nR: D/[9-0](N)\r\n")),
		   522);
	fixture_stop();

	CASE("signals: to= over a day gets 538, a day is accepted");
	up(&fx_rfc);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1,
				  "X: 1\r\nS: L/rg(to=4294967295)\r\n")), 538);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1,
				  "X: 2\r\nS: L/rg(to=86400000)\r\n")), 200);
	fixture_stop();
}

/* --------------------------------------------------- connection options */

static void option_tests(void)
{
	char id[MGCP_ID_MAX], rest[3072];
	const char *r;
	size_t n;

	CASE("mdcx: sendrecv with a zero address programs recvonly");
	offhook_up(&fx_rfc);
	r = ca_cmd("CRCX", EP1, "C: A1\r\nM: recvonly\r\nL: a:PCMU\r\n");
	conn_id_of(r, id, sizeof(id));
	mark_set();
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\nM: sendrecv\r\n\r\n"
		 SDP_HOLD, id);
	CHECK_UINT(code_of(ca_cmd("MDCX", EP1, rest)), 200);
	CHECK_UINT(hop_count(PHF_MEDIA_CONFIG_SET), 1);
	CHECK_UINT(hop_last(PHF_MEDIA_CONFIG_SET)->media.direction,
		   HAL_MEDIA_RECVONLY);
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\nM: sendonly\r\n", id);
	CHECK_UINT(code_of(ca_cmd("MDCX", EP1, rest)), 200);
	CHECK_UINT(hop_count(PHF_MEDIA_CONFIG_SET), 2);
	CHECK_UINT(hop_last(PHF_MEDIA_CONFIG_SET)->media.direction,
		   HAL_MEDIA_INACTIVE);
	fixture_stop();

	CASE("mdcx: a failed config_set on a move to T.38 restores the stream");
	offhook_up(&fx_rfc);
	call_up(id, sizeof(id));
	phf.config_set_fail = 1;
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\n\r\n" SDP_T38, id);
	CHECK_UINT(code_of(ca_cmd("MDCX", EP1, rest)), 400);
	CHECK_UINT(hop_count(PHF_MEDIA_STOP), 1);
	CHECK_UINT(hop_count(PHF_MEDIA_CONFIG_SET), 2);
	CHECK_UINT(hop_last(PHF_MEDIA_CONFIG_SET)->media.codec,
		   HAL_CODEC_PCMU);
	CHECK_UINT(hop_count(PHF_MEDIA_START), 1);
	CHECK(hop_index(PHF_MEDIA_START) > hop_index(PHF_MEDIA_STOP));
	CHECK(gw.ep[0].conn[0].media_started);
	CHECK_UINT(gw.ep[0].conn[0].media.codec, HAL_CODEC_PCMU);
	fixture_stop();

	CASE("mdcx: a failed start on a move to T.38 restores the stream");
	offhook_up(&fx_rfc);
	call_up(id, sizeof(id));
	phf.start_fail = 1;
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\n\r\n" SDP_T38, id);
	CHECK_UINT(code_of(ca_cmd("MDCX", EP1, rest)), 400);
	CHECK_UINT(hop_count(PHF_MEDIA_STOP), 1);
	CHECK_UINT(hop_count(PHF_MEDIA_CONFIG_SET), 2);
	CHECK_UINT(hop_last(PHF_MEDIA_CONFIG_SET)->media.codec,
		   HAL_CODEC_PCMU);
	CHECK_UINT(hop_count(PHF_MEDIA_START), 2);
	CHECK(gw.ep[0].conn[0].media_started);
	CHECK_UINT(gw.ep[0].conn[0].media.codec, HAL_CODEC_PCMU);
	fixture_stop();

	CASE("mdcx: a partial L: keeps the options in force");
	offhook_up(&fx_rfc);
	r = ca_cmd("CRCX", EP1, "C: A1\r\nM: recvonly\r\nL: p:30, a:PCMU\r\n");
	conn_id_of(r, id, sizeof(id));
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\nL: s:on\r\n", id);
	r = ca_cmd("MDCX", EP1, rest);
	CHECK_UINT(code_of(r), 200);
	CHECK(has(r, "a=ptime:30\r\n"));
	CHECK_UINT(gw.ep[0].conn[0].loc.codecs, 1);
	CHECK_UINT(gw.ep[0].conn[0].loc.codec[0].id, SDP_CODEC_PCMU);
	CHECK_UINT(gw.ep[0].conn[0].loc.ptime_ms, 30);
	fixture_stop();

	CASE("mdcx: the p: of the call agent wins over the answer's a=ptime");
	offhook_up(&fx_rfc);
	r = ca_cmd("CRCX", EP1, "C: A1\r\nM: recvonly\r\nL: p:30, a:PCMU\r\n");
	conn_id_of(r, id, sizeof(id));
	mark_set();
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\nM: sendrecv\r\n\r\n"
		 SDP_ANSWER "a=ptime:20\r\n", id);
	CHECK_UINT(code_of(ca_cmd("MDCX", EP1, rest)), 200);
	CHECK_UINT(hop_last(PHF_MEDIA_CONFIG_SET)->media.ptime_ms, 30);
	fixture_stop();

	CASE("crcx: t: sets the RTP DSCP alone, a bad t: gets 532");
	offhook_up(&fx_rfc);
	cfg.channel[0].rtcp_tos = 0x60;
	CHECK_UINT(code_of(ca_cmd("CRCX", EP1, "C: A1\r\nM: recvonly\r\n"
				  "L: t:ZZ\r\n")), 532);
	mark_set();
	r = ca_cmd("CRCX", EP1, "C: A1\r\nM: recvonly\r\nL: a:PCMU, t:B8\r\n"
		   "\r\n" SDP_ANSWER);
	CHECK_UINT(code_of(r), 200);
	CHECK_UINT(hop_count(PHF_MEDIA_CONFIG_SET), 1);
	CHECK_UINT(hop_last(PHF_MEDIA_CONFIG_SET)->media.rtp_dscp, 0xB8 >> 2);
	CHECK_UINT(hop_last(PHF_MEDIA_CONFIG_SET)->media.rtcp_dscp, 0x60 >> 2);
	fixture_stop();

	CASE("crcx: under ncs a t: with an ECN bit set gets 532");
	offhook_up(&fx_ncs);
	CHECK_UINT(code_of(ca_cmd("CRCX", EP1, "C: A1\r\nM: recvonly\r\n"
				  "L: t:B9\r\n")), 532);
	CHECK(!gw.ep[0].conn[0].used);
	CHECK_UINT(code_of(ca_cmd("CRCX", EP1, "C: A1\r\nM: recvonly\r\n"
				  "L: t:B8\r\n")), 200);
	fixture_stop();

	CASE("crcx: e: names the canceller in effect, not lec_enable alone");
	offhook_up(&fx_rfc);
	cfg.channel[0].lec_control = false;
	cfg.channel[0].lec_enable = false;
	CHECK_UINT(code_of(ca_cmd("CRCX", EP1, "C: A1\r\nM: recvonly\r\n"
				  "L: e:off\r\n")), 532);
	CHECK_UINT(code_of(ca_cmd("CRCX", EP1, "C: A1\r\nM: recvonly\r\n"
				  "L: e:on\r\n")), 200);
	fixture_stop();

	CASE("crcx: e: must name the setting of the line canceller");
	offhook_up(&fx_rfc);
	CHECK_UINT(code_of(ca_cmd("CRCX", EP1, "C: A1\r\nM: recvonly\r\n"
				  "L: e:off\r\n")), 532);
	CHECK(!gw.ep[0].conn[0].used);
	CHECK_UINT(code_of(ca_cmd("CRCX", EP1, "C: A1\r\nM: recvonly\r\n"
				  "L: e:on\r\n")), 200);
	fixture_stop();

	CASE("aucx: LC and RC fit with a long remote description");
	offhook_up(&fx_rfc);
	r = ca_cmd("CRCX", EP1, "C: A1\r\nM: recvonly\r\nL: a:PCMU\r\n");
	conn_id_of(r, id, sizeof(id));
	n = (size_t)snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\n"
			     "M: sendrecv\r\n\r\n" SDP_ANSWER, id);
	while (n + 64 < sizeof(rest) && n < 1900)
		n += (size_t)snprintf(rest + n, sizeof(rest) - n,
				      "a=x-pad:0123456789012345678901234567"
				      "89\r\n");
	snprintf(rest + n, sizeof(rest) - n, "a=x-end:1\r\n");
	CHECK_UINT(code_of(ca_cmd("MDCX", EP1, rest)), 200);
	CHECK(gw.ep[0].conn[0].rcd_len > 1800);
	snprintf(rest, sizeof(rest), "I: %s\r\nF: LC,RC\r\n", id);
	r = ca_cmd("AUCX", EP1, rest);
	CHECK_UINT(code_of(r), 200);
	CHECK(has(r, "m=audio 16384") && has(r, "m=audio 30000"));
	CHECK(has(r, "a=x-end:1\r\n"));
	fixture_stop();
}

/* ---------------------------------------------- signals and the quarantine */

static void queue_tests(void)
{
	char name[320], rest[512];
	unsigned int i;

	CASE("signals: nine digits in one S: play nine");
	offhook_up(&fx_rfc);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 49\r\nS: D/1, D/2, D/3, "
				  "D/4, D/5, D/6, D/7, D/8, D/9\r\n")), 200);
	CHECK_UINT(hop_count(PHF_DIGIT_PLAY), 9);
	CHECK(has(ca_cmd("AUEP", EP1, "F: S\r\n"),
		  "S: D/2,D/3,D/4,D/5,D/6,D/7,D/8,D/9\r\n"));
	{
		char many[256];
		struct sip_buf b;

		sip_buf_init(&b, many, sizeof(many));
		sip_buf_add(&b, "X: 4F\r\nS: D/0");
		/* 33 digits, one over the MGCP_BR_MAX queue */
		for (i = 1; i <= 32; i++)
			sip_buf_add(&b, ", D/1");
		sip_buf_add(&b, "\r\n");
		CHECK(sip_buf_ok(&b));
		CHECK_UINT(code_of(ca_cmd("RQNT", EP1, many)), 510);
	}
	fixture_stop();

	CASE("signals: a long caller name keeps its head");
	up(&fx_rfc);
	memset(name, 'A', 300);
	name[300] = '\0';
	snprintf(rest, sizeof(rest), "X: 4A\r\nS: L/ci(10/01/09/30, 5551234, "
		 "\"%s\")\r\n", name);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, rest)), 200);
	CHECK_UINT(hop_count(PHF_CID_SEND), 1);
	CHECK(hop_last(PHF_CID_SEND)->cid.name[0] == 'A');
	CHECK_UINT(strlen(hop_last(PHF_CID_SEND)->cid.name),
		   sizeof(hop_last(PHF_CID_SEND)->cid.name) - 1);
	fixture_stop();

	CASE("signals: a flash cancels the pending digits");
	offhook_up(&fx_rfc);
	ca_cmd("RQNT", EP1, "X: 4B\r\nR: L/hf(N)\r\nS: D/1, D/2, D/3\r\n");
	mark_set();
	ev_hook(0, HAL_HOOK_FLASH);
	CHECK_UINT(hop_count(PHF_DIGIT_PLAY_CANCEL), 1);
	CHECK(!has(ca_cmd("AUEP", EP1, "F: S\r\n"), "D/"));
	fixture_stop();

	CASE("signals: a TO signal named again keeps its timer");
	offhook_up(&fx_rfc);
	ca_cmd("RQNT", EP1, "X: 4C\r\nR: L/oc(N)\r\nS: L/dl\r\n");
	fake_advance(10000);
	mark_set();
	ca_cmd("RQNT", EP1, "X: 4D\r\nR: L/oc(N)\r\nS: L/dl\r\n");
	CHECK_UINT(hop_count(PHF_TONE_PLAY), 0);
	fake_advance(6000);
	CHECK(has(sent("NTFY"), "O: L/oc(L/dl)\r\n"));
	fixture_stop();

	CASE("signals: a service cycle after lsa restores the polarity");
	up(&fx_rfc);
	ca_cmd("RQNT", EP1, "X: 4E\r\nS: L/lsa\r\n");
	mark_set();
	mgcp_ep_service(&gw, 0, false, MGCP_EP_REASON_OOS);
	CHECK_UINT(hop_count(PHF_BATTERY_SET), 1);
	CHECK_UINT(hop_last(PHF_BATTERY_SET)->val, PROTO_BATT_ACTIVE);
	fixture_stop();

	CASE("quarantine: a replayed T continues the digit map");
	offhook_up(&fx_rfc);
	memset(gw.ep[0].quar, 0, 2 * sizeof(gw.ep[0].quar[0]));
	gw.ep[0].quar[0].kind = MGCP_EVK_DIGIT;
	gw.ep[0].quar[0].digit = '9';
	gw.ep[0].quar[1].kind = MGCP_EVK_T;
	gw.ep[0].nquar = 2;
	mark_set();
	ca_cmd("RQNT", EP1, "X: D0\r\nR: D/[0-9T](D)\r\nD: (9T11)\r\n"
	       "Q: process\r\n");
	CHECK_UINT(sent_count("NTFY"), 0);
	ev_digits(0, "11");
	CHECK(has(sent("NTFY"), "O: D/9,D/T,D/1,D/1\r\n"));
	fixture_stop();

	CASE("quarantine: an overflowing requeue keeps the oldest events");
	offhook_up(&fx_rfc);
	memset(gw.ep[0].obs, 0, 10 * sizeof(gw.ep[0].obs[0]));
	memset(gw.ep[0].quar, 0, 10 * sizeof(gw.ep[0].quar[0]));
	/* the quarantined events differ, so the ones kept are visible */
	for (i = 0; i < 10; i++) {
		gw.ep[0].obs[i].kind = MGCP_EVK_DIGIT;
		gw.ep[0].obs[i].digit = (char)('0' + i);
		gw.ep[0].quar[i].kind = i ? MGCP_EVK_DIGIT : MGCP_EVK_HF;
		gw.ep[0].quar[i].digit = (char)('0' + i);
	}
	gw.ep[0].nobs = 10;
	gw.ep[0].nquar = 10;
	mark_set();
	ca_cmd("RQNT", EP1, "X: D1\r\nR: D/[0-9](A), L/hf(N)\r\n");
	CHECK(has(sent("NTFY"), "O: D/0,D/1,D/2,D/3,D/4,D/5,D/6,D/7,D/8,"
		  "D/9,L/hf\r\n"));
	CHECK_UINT(gw.ep[0].quar_dropped, 4);
	/* behind the NTFY wait the quarantined digits 1 to 5, not 6 to 9 */
	CHECK_UINT(gw.ep[0].nquar, 5);
	CHECK(gw.ep[0].quar[0].digit == '1' && gw.ep[0].quar[4].digit == '5');
	fixture_stop();

	CASE("quarantine: a new request requeues the accumulated events");
	offhook_up(&fx_rfc);
	ca_cmd("RQNT", EP1, "X: DA\r\nR: D/[0-9](A)\r\n");
	ev_digits(0, "12");
	mark_set();
	ca_cmd("RQNT", EP1, "X: D3\r\nR: D/[0-9](N)\r\n");
	CHECK(has(sent("NTFY"), "X: D3\r\n"));
	CHECK(has(sent("NTFY"), "O: D/1\r\n"));
	fixture_stop();

	CASE("quarantine: an event while disconnected waits in the buffer");
	offhook_up(&fx_rfc);
	ca_cmd("RQNT", EP1, "X: D4\r\nR: L/hf(N), L/hu(N)\r\n");
	ev_hook(0, HAL_HOOK_FLASH);
	fake_advance(30000);
	CHECK(mgcp_txn_disc_active(&mgr));
	/* the request ends the notification state of the lost NTFY */
	ca_cmd("RQNT", EP1, "X: D5\r\nR: L/hf(N), L/hu(N)\r\n");
	CHECK(!gw.ep[0].notif);
	mark_set();
	ev_hook(0, HAL_HOOK_RELEASE);
	CHECK_UINT(sent_count("NTFY"), 0);
	CHECK_UINT(gw.ep[0].nquar, 1);
	fixture_stop();
}

/* ------------------------------------------- host limits and the record */

/* An RQNT whose head is followed by an S: of n D/1 digits. */
static const char *digit_rqnt(char *buf, size_t len, const char *head,
			      unsigned int n)
{
	struct sip_buf b;
	unsigned int i;

	sip_buf_init(&b, buf, len);
	sip_buf_add(&b, head);
	sip_buf_add(&b, "S: D/1");
	for (i = 1; i < n; i++)
		sip_buf_add(&b, ", D/1");
	sip_buf_add(&b, "\r\n");

	return sip_buf_ok(&b) ? buf : "";
}

#define SDP_PORT_MAX							\
	"v=0\r\n"							\
	"o=- 1 4 IN IP4 192.0.2.50\r\n"					\
	"s=-\r\n"							\
	"c=IN IP4 192.0.2.50\r\n"					\
	"t=0 0\r\n"							\
	"m=audio 65535 RTP/AVP 0\r\n"

static void host_tests(void)
{
	char id[MGCP_ID_MAX], rest[1024], buf[512];
	const struct fake_send *rec;
	uint32_t rsip_id;
	const char *r;

	CASE("signals: the digits the host takes are accepted");
	offhook_up(&fx_rfc);
	phf.digit_q_max = 4;
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, digit_rqnt(buf, sizeof(buf),
				  "X: 61\r\n", 5))), 200);
	CHECK_UINT(hop_count(PHF_DIGIT_PLAY), 5);
	CHECK_UINT(hop_count(PHF_DIGIT_PLAY_CANCEL), 0);
	fixture_stop();

	CASE("signals: a refused digit gets 403, the rest of the request "
	     "stands");
	offhook_up(&fx_rfc);
	phf.digit_q_max = 4;
	ca_cmd("RQNT", EP1, "X: 62\r\nD: (xx)\r\nS: D/1, D/2\r\n");
	mark_set();
	r = ca_cmd("RQNT", EP1,
		   digit_rqnt(buf, sizeof(buf),
			      "X: 63\r\nR: L/hu(N)\r\nD: (9)\r\n", 6));
	CHECK_UINT(code_of(r), 403);
	CHECK(!strcmp(gw.ep[0].rid, "63"));
	CHECK_UINT(gw.ep[0].nreq, 1);
	CHECK(!strcmp(gw.ep[0].dmap, "9"));
	CHECK_UINT(hop_count(PHF_DIGIT_PLAY_CANCEL), 2);
	CHECK(!has(ca_cmd("AUEP", EP1, "F: S\r\n"), "D/"));
	fixture_stop();
	offhook_up(&fx_ncs);
	phf.digit_q_max = 4;
	r = ca_cmd("RQNT", EP1, "X: 64\r\nS: L/1, L/1, L/1, L/1, L/1, L/1\r\n");
	CHECK_UINT(code_of(r), 400);
	CHECK(!strcmp(gw.ep[0].rid, "64"));
	fixture_stop();

	CASE("signals: a digit the line refuses gets 403, and 400 under ncs");
	offhook_up(&fx_rfc);
	phf.accept = false;
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 69\r\nS: D/1\r\n")), 403);
	CHECK_UINT(hop_count(PHF_DIGIT_PLAY), 1);
	fixture_stop();
	offhook_up(&fx_ncs);
	phf.accept = false;
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 69\r\nS: L/1\r\n")), 400);
	fixture_stop();

	CASE("signals: the tone stops before the first digit plays");
	offhook_up(&fx_rfc);
	ca_cmd("RQNT", EP1, "X: 6A\r\nS: L/dl\r\n");
	mark_set();
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 6B\r\nS: D/1, D/2\r\n")),
		   200);
	CHECK(hop_index(PHF_TONE_STOP) >= 0);
	CHECK(hop_index(PHF_TONE_STOP) < hop_index(PHF_DIGIT_PLAY));
	fixture_stop();

	CASE("crcx: CRCX with refused digits leaves no connection");
	offhook_up(&fx_rfc);
	phf.digit_q_max = 1;
	r = ca_cmd("CRCX", EP1, digit_rqnt(buf, sizeof(buf), "C: A1\r\n"
		   "M: recvonly\r\nX: 65\r\n", 3));
	CHECK_UINT(code_of(r), 403);
	CHECK(!has(r, "I: "));
	CHECK(!gw.ep[0].conn[0].used && !gw.ep[0].conn[1].used);
	CHECK(!strcmp(gw.ep[0].rid, "65"));
	CHECK_UINT(phf.n[PHF_NET_PORT_ALLOC], 1);
	CHECK_UINT(phf.n[PHF_NET_PORT_FREE], 1);
	CHECK_UINT(phf.n[PHF_CDR_CALL_STARTED], 0);
	CHECK_UINT(phf.n[PHF_CDR_CALL_ENDED], 0);
	fixture_stop();
	offhook_up(&fx_ncs);
	phf.digit_q_max = 1;
	r = ca_cmd("CRCX", EP1, "C: A1\r\nM: recvonly\r\nX: 65\r\n"
		   "S: L/1, L/2, L/3\r\n");
	CHECK_UINT(code_of(r), 400);
	CHECK(!has(r, "I: "));
	CHECK(!gw.ep[0].conn[0].used);
	fixture_stop();

	CASE("crcx: a failed program plays no digit");
	offhook_up(&fx_rfc);
	phf.start_fail = 1;
	CHECK_UINT(code_of(ca_cmd("CRCX", EP1, "C: A1\r\nM: sendrecv\r\n"
				  "X: 6C\r\nS: D/1, D/2\r\n\r\n" SDP_ANSWER)),
		   400);
	CHECK_UINT(hop_count(PHF_DIGIT_PLAY), 0);
	CHECK(!gw.ep[0].conn[0].used);
	fixture_stop();

	CASE("mdcx: a refused digit keeps the new media");
	offhook_up(&fx_rfc);
	call_up(id, sizeof(id));
	phf.digit_q_max = 1;
	snprintf(rest, sizeof(rest),
		 "C: A1\r\nI: %s\r\nX: 66\r\n%s\r\n" SDP_PCMA, id,
		 digit_rqnt(buf, sizeof(buf), "", 3));
	CHECK_UINT(code_of(ca_cmd("MDCX", EP1, rest)), 403);
	CHECK_UINT(gw.ep[0].conn[0].media.codec, HAL_CODEC_PCMA);
	CHECK(gw.ep[0].conn[0].media_started);
	fixture_stop();

	CASE("crcx: the digits play after the stream starts");
	offhook_up(&fx_rfc);
	CHECK_UINT(code_of(ca_cmd("CRCX", EP1, "C: A1\r\nM: sendrecv\r\n"
				  "X: 6D\r\nS: D/1\r\n\r\n" SDP_ANSWER)), 200);
	CHECK(hop_index(PHF_MEDIA_START) >= 0);
	CHECK(hop_index(PHF_MEDIA_START) < hop_index(PHF_DIGIT_PLAY));
	fixture_stop();

	CASE("mdcx: the digits play after the new media");
	offhook_up(&fx_rfc);
	call_up(id, sizeof(id));
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\nX: 6E\r\nS: D/1\r\n\r\n"
		 SDP_PCMA, id);
	CHECK_UINT(code_of(ca_cmd("MDCX", EP1, rest)), 200);
	CHECK(hop_index(PHF_MEDIA_CONFIG_SET) >= 0);
	CHECK(hop_index(PHF_MEDIA_CONFIG_SET) < hop_index(PHF_DIGIT_PLAY));
	fixture_stop();

	CASE("dlcx: the digits play after the stream stops");
	offhook_up(&fx_rfc);
	call_up(id, sizeof(id));
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\nX: 6F\r\nS: D/1\r\n",
		 id);
	CHECK_UINT(code_of(ca_cmd("DLCX", EP1, rest)), 250);
	CHECK(hop_index(PHF_MEDIA_STOP) >= 0);
	CHECK(hop_index(PHF_MEDIA_STOP) < hop_index(PHF_DIGIT_PLAY));
	fixture_stop();

	CASE("embedded: the digits play after the tone stops");
	offhook_up(&fx_rfc);
	ca_cmd("RQNT", EP1, "X: 70\r\nR: L/hf(K,E(S(D/1)))\r\nS: L/dl\r\n");
	mark_set();
	ev_hook(0, HAL_HOOK_FLASH);
	CHECK_UINT(hop_count(PHF_DIGIT_PLAY), 1);
	CHECK(hop_index(PHF_TONE_STOP) >= 0);
	CHECK(hop_index(PHF_TONE_STOP) < hop_index(PHF_DIGIT_PLAY));
	fixture_stop();

	CASE("crcx: the digits and the next cancel name the new connection");
	offhook_up(&fx_rfc);
	ca_cmd("CRCX", EP1, "C: A1\r\nM: inactive\r\n");
	ca_cmd("CRCX", EP1, "C: B2\r\nM: inactive\r\n");
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\n", gw.ep[0].conn[0].id);
	ca_cmd("DLCX", EP1, rest);
	mark_set();
	CHECK_UINT(code_of(ca_cmd("CRCX", EP1, "C: C3\r\nM: inactive\r\n"
				  "X: 71\r\nS: D/1, D/2\r\n")), 200);
	CHECK(gw.ep[0].conn[0].used);
	CHECK_UINT(hop_last(PHF_DIGIT_PLAY)->stream, 0);
	ca_cmd("RQNT", EP1, "X: 72\r\n");
	CHECK_UINT(hop_last(PHF_DIGIT_PLAY_CANCEL)->stream, 0);
	fixture_stop();

	CASE("dlcx: a new request cancels the pending digits before the stop");
	offhook_up(&fx_rfc);
	call_up(id, sizeof(id));
	ca_cmd("RQNT", EP1, "X: 75\r\nS: D/1, D/2, D/3\r\n");
	mark_set();
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\nX: 76\r\n", id);
	CHECK_UINT(code_of(ca_cmd("DLCX", EP1, rest)), 250);
	CHECK_UINT(hop_last(PHF_DIGIT_PLAY_CANCEL)->stream, 0);
	CHECK(hop_index(PHF_DIGIT_PLAY_CANCEL) >= 0);
	CHECK(hop_index(PHF_DIGIT_PLAY_CANCEL) < hop_index(PHF_MEDIA_STOP));
	fixture_stop();

	CASE("service: the pending digits cancel before the streams stop");
	offhook_up(&fx_rfc);
	call_up(id, sizeof(id));
	ca_cmd("RQNT", EP1, "X: 73\r\nS: D/1, D/2, D/3\r\n");
	mark_set();
	mgcp_ep_service(&gw, 0, false, MGCP_EP_REASON_OOS);
	CHECK(hop_index(PHF_DIGIT_PLAY_CANCEL) >= 0);
	CHECK(hop_index(PHF_DIGIT_PLAY_CANCEL) < hop_index(PHF_MEDIA_STOP));
	fixture_stop();

	CASE("dlcx: the digits go on the stream that stays");
	offhook_up(&fx_rfc);
	ca_cmd("CRCX", EP1, "C: A1\r\nM: inactive\r\n");
	ca_cmd("CRCX", EP1, "C: B2\r\nM: inactive\r\n");
	mark_set();
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\nX: 67\r\n"
		 "S: D/1, D/2\r\n", gw.ep[0].conn[0].id);
	CHECK_UINT(code_of(ca_cmd("DLCX", EP1, rest)), 250);
	CHECK_UINT(hop_count(PHF_DIGIT_PLAY), 2);
	CHECK_UINT(hop_last(PHF_DIGIT_PLAY)->stream, 1);
	ca_cmd("RQNT", EP1, "X: 68\r\n");
	CHECK_UINT(hop_count(PHF_DIGIT_PLAY_CANCEL), 1);
	CHECK_UINT(hop_last(PHF_DIGIT_PLAY_CANCEL)->stream, 1);
	fixture_stop();

	CASE("digit map: the timers follow the channel interdigit options");
	offhook_up(&fx_rfc);
	cfg.channel[0].interdigit_s = 3;
	cfg.channel[0].interdigit_long_s = 10;
	rqnt_digits("xxT|xxxx", "D/[0-9#*T](D)");
	ev_digits(0, "12");
	CHECK_UINT(fake_timer_due(&gw.ep[0].digit_timer), fk.now + 3000);
	fixture_stop();
	offhook_up(&fx_rfc);
	cfg.channel[0].interdigit_long_s = 10;
	rqnt_digits("xxxx", "D/[0-9#*T](D)");
	ev_digit(0, '5');
	CHECK_UINT(fake_timer_due(&gw.ep[0].digit_timer), fk.now + 10000);
	fixture_stop();

	CASE("signals: a service cycle cancels the pending digits");
	offhook_up(&fx_rfc);
	ca_cmd("RQNT", EP1, "X: 64\r\nS: D/1, D/2, D/3\r\n");
	mark_set();
	mgcp_ep_service(&gw, 0, false, MGCP_EP_REASON_OOS);
	CHECK_UINT(hop_count(PHF_DIGIT_PLAY_CANCEL), 1);
	fixture_stop();

	CASE("restart: a send failure in the retransmission arms the retry");
	fixture_start(&fx_rfc);
	mgcp_ep_start(&gw);
	fake_advance(9256);
	CHECK_UINT(sent_count("RSIP"), 1);
	fk.send_fail = true;
	fake_advance(1000);
	fk.send_fail = false;
	mark_set();
	fake_advance(10001);
	CHECK(sent_to("RSIP", NULL) != NULL);
	ca_answer(sent_to("RSIP", NULL) ? sent_to("RSIP", NULL)->buf : "", 200,
		  NULL);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 1\r\n")), 200);
	fixture_stop();

	CASE("restarting: ncs puts the RSIP in front of a 511");
	fixture_start(&fx_ncs);
	mgcp_ep_start(&gw);
	r = ca_cmd("RQNT", EP1, "X: 1\r\nX+vendor: 1\r\n");
	CHECK(!strncmp(r, "RSIP ", 5) && has(r, "\r\n.\r\n511 "));
	fixture_stop();

	CASE("mdcx: a description the stream cannot take touches no stream");
	offhook_up(&fx_rfc);
	call_up(id, sizeof(id));
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\n\r\n" SDP_PORT_MAX, id);
	CHECK_UINT(code_of(ca_cmd("MDCX", EP1, rest)), 400);
	CHECK_UINT(hop_count(PHF_MEDIA_CONFIG_SET), 0);
	CHECK_UINT(hop_count(PHF_MEDIA_START), 0);
	CHECK_UINT(hop_count(PHF_MEDIA_STOP), 0);
	fixture_stop();

	CASE("mdcx: a refused stop on a move to T.38 still moves the stream");
	offhook_up(&fx_rfc);
	call_up(id, sizeof(id));
	phf.stop_fail = 1;
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\n\r\n" SDP_T38, id);
	CHECK_UINT(code_of(ca_cmd("MDCX", EP1, rest)), 200);
	CHECK_UINT(hop_count(PHF_MEDIA_STOP), 1);
	CHECK_UINT(hop_count(PHF_MEDIA_CONFIG_SET), 1);
	CHECK_UINT(hop_count(PHF_MEDIA_START), 1);
	CHECK(gw.ep[0].conn[0].media_started);
	CHECK_UINT(gw.ep[0].conn[0].media.codec, HAL_CODEC_T38);
	fixture_stop();

	CASE("mdcx: a restore that fails leaves the record not started");
	offhook_up(&fx_rfc);
	call_up(id, sizeof(id));
	phf.config_set_fail = 2;
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\n\r\n" SDP_T38, id);
	CHECK_UINT(code_of(ca_cmd("MDCX", EP1, rest)), 400);
	CHECK(!gw.ep[0].conn[0].media_started);
	fixture_stop();

	CASE("dlcx: a configured stream that a failed restore left stops");
	offhook_up(&fx_rfc);
	call_up(id, sizeof(id));
	phf.start_fail = 2;
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\n\r\n" SDP_T38, id);
	CHECK_UINT(code_of(ca_cmd("MDCX", EP1, rest)), 400);
	CHECK(!gw.ep[0].conn[0].media_started);
	mark_set();
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\n", id);
	CHECK_UINT(code_of(ca_cmd("DLCX", EP1, rest)), 250);
	CHECK_UINT(hop_count(PHF_MEDIA_STOP), 1);
	fixture_stop();

	CASE("mdcx: a failed config_set without a move restores the stream");
	offhook_up(&fx_rfc);
	call_up(id, sizeof(id));
	phf.config_set_fail = 1;
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\nM: recvonly\r\n", id);
	CHECK_UINT(code_of(ca_cmd("MDCX", EP1, rest)), 400);
	CHECK_UINT(hop_count(PHF_MEDIA_CONFIG_SET), 2);
	CHECK(!memcmp(&hop_last(PHF_MEDIA_CONFIG_SET)->media,
		      &gw.ep[0].conn[0].media, sizeof(struct hal_media)));
	CHECK_UINT(gw.ep[0].conn[0].mode, MGCP_MODE_SENDRECV);
	CHECK(gw.ep[0].conn[0].media_started);
	fixture_stop();

	CASE("mdcx: a failed start without a move restores the stream");
	offhook_up(&fx_rfc);
	call_up(id, sizeof(id));
	phf.start_fail = 2;
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\n\r\n" SDP_T38, id);
	ca_cmd("MDCX", EP1, rest);
	CHECK(!gw.ep[0].conn[0].media_started);
	mark_set();
	phf.start_fail = 1;
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\nX: 74\r\n", id);
	CHECK_UINT(code_of(ca_cmd("MDCX", EP1, rest)), 400);
	CHECK_UINT(hop_count(PHF_MEDIA_START), 1);
	CHECK_UINT(hop_count(PHF_MEDIA_CONFIG_SET), 1);
	CHECK(!gw.ep[0].conn[0].media_started);
	fixture_stop();

	CASE("mdcx: a new offer without a: drops the a: in force");
	offhook_up(&fx_rfc);
	call_up(id, sizeof(id));
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\nL: s:on\r\n\r\n"
		 SDP_PCMA, id);
	CHECK_UINT(code_of(ca_cmd("MDCX", EP1, rest)), 200);
	CHECK_UINT(gw.ep[0].conn[0].media.codec, HAL_CODEC_PCMA);
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\nL: s:off\r\n", id);
	CHECK_UINT(code_of(ca_cmd("MDCX", EP1, rest)), 200);
	CHECK_UINT(gw.ep[0].conn[0].media.codec, HAL_CODEC_PCMA);
	fixture_stop();

	CASE("aucx: L: lists every option supplied, merged");
	offhook_up(&fx_rfc);
	r = ca_cmd("CRCX", EP1, "C: A1\r\nM: recvonly\r\n"
		   "L: a:PCMU, gc:0, r:be, nt:IN, b:64\r\n");
	conn_id_of(r, id, sizeof(id));
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\nL: s:on\r\n", id);
	CHECK_UINT(code_of(ca_cmd("MDCX", EP1, rest)), 200);
	snprintf(rest, sizeof(rest), "I: %s\r\nF: L\r\n", id);
	CHECK(has(ca_cmd("AUCX", EP1, rest),
		  "\r\nL: a:PCMU, gc:0, r:be, nt:IN, b:64, s:on\r\n"));
	fixture_stop();

	CASE("aucx: e: is in the audit and survives a partial L:");
	{
		bool ec, nlp;

		offhook_up(&fx_rfc);
		cfg_channel_lec_effective(&cfg.channel[0], &ec, &nlp);
		snprintf(rest, sizeof(rest), "C: A1\r\nM: recvonly\r\n"
			 "L: e:%s\r\n", ec ? "on" : "off");
		r = ca_cmd("CRCX", EP1, rest);
		conn_id_of(r, id, sizeof(id));
		snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\nL: s:on\r\n",
			 id);
		CHECK_UINT(code_of(ca_cmd("MDCX", EP1, rest)), 200);
		snprintf(rest, sizeof(rest), "I: %s\r\nF: L\r\n", id);
		snprintf(buf, sizeof(buf), "\r\nL: e:%s, s:on\r\n",
			 ec ? "on" : "off");
		CHECK(has(ca_cmd("AUCX", EP1, rest), buf));
		CHECK_UINT(gw.ep[0].conn[0].lopt.ec, ec);
		fixture_stop();
	}

	CASE("lco: t:B9 is taken under rfc3435 and refused under ncs");
	offhook_up(&fx_rfc);
	CHECK_UINT(code_of(ca_cmd("CRCX", EP1, "C: A1\r\nM: recvonly\r\n"
				  "L: t:B9\r\n")), 200);
	fixture_stop();
	offhook_up(&fx_ncs);
	CHECK_UINT(code_of(ca_cmd("CRCX", EP1, "C: A1\r\nM: recvonly\r\n"
				  "L: t:B9\r\n")), 532);
	fixture_stop();

	CASE("restart: a send failure of an endpoint RSIP arms the retry");
	up(&fx_rfc);
	mgcp_ep_service(&gw, 0, false, MGCP_EP_REASON_OOS);
	ca_answer(sent("RSIP"), 200, NULL);
	mgcp_ep_service(&gw, 0, true, 0);
	CHECK(sent_to("RSIP", NULL) != NULL);
	fk.send_fail = true;
	fake_advance(1000);
	fk.send_fail = false;
	mark_set();
	fake_advance(10001);
	CHECK(sent_to("RSIP", NULL) && has(sent_to("RSIP", NULL)->buf,
					   " " EP1 " ") &&
	      has(sent_to("RSIP", NULL)->buf, "RM: restart\r\n"));
	fixture_stop();

	CASE("service: before the start a line leaves service with no message");
	fixture_start(&fx_rfc);
	mgcp_ep_service(&gw, 1, false, MGCP_EP_REASON_OOS);
	CHECK_UINT(fk.sends, 0);
	CHECK_UINT(gw.ep[1].svc, MGCP_EP_OUT_OF_SERVICE);
	mgcp_ep_start(&gw);
	fake_advance(10001);
	CHECK(sent_count("RSIP") >= 1);
	CHECK(has(sent("RSIP"), " " EP1 " ") &&
	      has(sent("RSIP"), "RM: restart\r\n"));
	CHECK(sent_to("RSIP", NULL) &&
	      !has(sent_to("RSIP", NULL)->buf, " " EP2 " "));
	fixture_stop();

	CASE("stop: RSIP graceful with RD: for an endpoint with a connection");
	up(&fx_rfc);
	ca_cmd("CRCX", EP1, "C: A1\r\nM: recvonly\r\n");
	mark_set();
	mgcp_ep_stop_begin(&gw, 3);
	CHECK_UINT(sent_count("RSIP"), 1);
	CHECK(has(sent("RSIP"), " " EP1 " ") &&
	      has(sent("RSIP"), "RM: graceful\r\n") &&
	      has(sent("RSIP"), "RD: 3\r\n"));
	mark_set();
	mgcp_ep_service(&gw, 0, false, MGCP_EP_REASON_OOS);
	CHECK(has(sent("RSIP"), "RM: forced\r\n") &&
	      !has(sent("RSIP"), "RD:"));
	CHECK_UINT(sent_count("DLCX"), 1);
	fixture_stop();

	CASE("stop: no restart RSIP leaves after the stop began");
	fixture_start(&fx_rfc);
	mgcp_ep_start(&gw);
	mgcp_ep_stop_begin(&gw, 3);
	fake_advance(20001);
	CHECK_UINT(sent_count("RSIP"), 0);
	fixture_stop();

	CASE("stop: a restart RSIP answered 4xx or 521 after the stop began "
	     "goes out no more");
	fixture_start(&fx_rfc);
	mgcp_ep_start(&gw);
	fake_advance(10001);
	r = sent("RSIP");
	mgcp_ep_stop_begin(&gw, 3);
	mark_set();
	ca_answer(r, 400, NULL);
	fake_advance(20001);
	CHECK_UINT(sent_count("RSIP"), 0);
	fixture_stop();
	fixture_start(&fx_rfc);
	mgcp_ep_start(&gw);
	fake_advance(10001);
	r = sent("RSIP");
	mgcp_ep_stop_begin(&gw, 3);
	mark_set();
	ca_answer(r, 521, "N: ca2@[192.0.2.2]:2727\r\n");
	fake_advance(20001);
	CHECK_UINT(sent_count("RSIP"), 0);
	fixture_stop();

	CASE("stop: a line back in service after the stop began sends no "
	     "restart RSIP");
	up(&fx_rfc);
	mgcp_ep_service(&gw, 0, false, MGCP_EP_REASON_OOS);
	ca_answer(sent("RSIP"), 200, NULL);
	mgcp_ep_stop_begin(&gw, 3);
	mark_set();
	mgcp_ep_service(&gw, 0, true, 0);
	fake_advance(20001);
	CHECK_UINT(sent_count("RSIP"), 0);
	fixture_stop();

	CASE("stop: an attempt of the disconnected procedure after the stop "
	     "began sends nothing");
	offhook_up(&fx_rfc);
	ca_cmd("RQNT", EP1, "X: B0\r\nR: L/hf(N), L/hu(N)\r\n");
	ev_hook(0, HAL_HOOK_FLASH);
	mark_set();
	fake_advance(15000);
	CHECK(mgcp_txn_disc_active(&mgr));
	rsip_id = txid_of(sent("RSIP"));
	CHECK(rsip_id != 0);
	mgcp_ep_stop_begin(&gw, 3);
	mark_set();
	fake_advance(700000);
	CHECK(sent_to("RSIP", NULL) != NULL);
	CHECK_UINT(sent_to("RSIP", NULL) ?
		   txid_of(sent_to("RSIP", NULL)->buf) : 0, rsip_id);
	CHECK_UINT(mgcp_txn_count(&mgr), 0);
	fixture_stop();

	CASE("stop: an endpoint's own RSIP of the procedure answered 521 after "
	     "the stop began goes out no more");
	offhook_up(&fx_rfc);
	ca_cmd("RQNT", EP1, "X: C0\r\nR: L/hf(N), L/hu(N)\r\n");
	ca_cmd("RQNT", EP2, "X: C1\r\nN: ca2@[192.0.2.2]:2727\r\n"
	       "R: L/hd(N)\r\n");
	ev_hook(0, HAL_HOOK_FLASH);
	ev_hook(1, HAL_HOOK_SEIZE);
	fake_advance(45000);
	rec = sent_to("RSIP", "192.0.2.2");
	CHECK(rec && has(rec->buf, " " EP2 " "));
	mgcp_ep_stop_begin(&gw, 3);
	mark_set();
	ca_answer(rec ? rec->buf : "", 521, "N: ca3@[192.0.2.3]:2727\r\n");
	CHECK_UINT(sent_count("RSIP"), 0);
	fixture_stop();

	CASE("hook: a digit toward the phone passes in service and not out of "
	     "service");
	up(&fx_rfc);
	CHECK(mgcp_ep_digit_play_allowed(&gw, 0));
	mgcp_ep_service(&gw, 0, false, MGCP_EP_REASON_OOS);
	CHECK(!mgcp_ep_digit_play_allowed(&gw, 0));
	CHECK(mgcp_ep_digit_play_allowed(&gw, 1));
	fixture_stop();

	CASE("digits: the cancel names the stream the digits went to after "
	     "the connections changed");
	offhook_up(&fx_rfc);
	r = ca_cmd("CRCX", EP1, "C: A1\r\nM: recvonly\r\n");
	conn_id_of(r, id, sizeof(id));
	ca_cmd("CRCX", EP1, "C: A1\r\nM: recvonly\r\n");
	r = ca_cmd("RQNT", EP1, "X: 71\r\nS: D/1, D/2, D/3\r\n");
	CHECK_UINT(code_of(r), 200);
	CHECK_UINT(hop_last(PHF_DIGIT_PLAY)->stream, 0);
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\n", id);
	CHECK_UINT(code_of(ca_cmd("DLCX", EP1, rest)), 250);
	CHECK(!gw.ep[0].conn[0].used && gw.ep[0].conn[1].used);
	mark_set();
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 72\r\nS: L/dl\r\n")), 200);
	CHECK_UINT(hop_count(PHF_DIGIT_PLAY_CANCEL), 1);
	CHECK_UINT(hop_last(PHF_DIGIT_PLAY_CANCEL)->stream, 0);
	fixture_stop();

	CASE("service: back in service with no address sends the RSIP later");
	up(&fx_rfc);
	mgcp_ep_service(&gw, 0, false, MGCP_EP_REASON_OOS);
	ca_answer(sent("RSIP"), 200, NULL);
	fk.dst_fail = true;
	mgcp_ep_service(&gw, 0, true, 0);
	fk.dst_fail = false;
	mark_set();
	fake_advance(10001);
	CHECK(sent_to("RSIP", NULL) && has(sent_to("RSIP", NULL)->buf,
					   " " EP1 " ") &&
	      has(sent_to("RSIP", NULL)->buf, "RM: restart\r\n"));
	fixture_stop();
}

static void record_tests(void)
{
	char id[MGCP_ID_MAX], rest[1024], name[320], big[600];
	const struct fake_send *rec;
	struct sip_buf b;
	const char *r;
	unsigned int i;

	CASE("mdcx: a new offer meets the a: of the command, not the a: kept");
	offhook_up(&fx_rfc);
	call_up(id, sizeof(id));
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\nL: s:on\r\n\r\n"
		 SDP_PCMA, id);
	r = ca_cmd("MDCX", EP1, rest);
	CHECK_UINT(code_of(r), 200);
	CHECK(has(r, "m=audio 16384 RTP/AVP 8"));
	CHECK_UINT(hop_last(PHF_MEDIA_CONFIG_SET)->media.codec, HAL_CODEC_PCMA);
	fixture_stop();

	CASE("aucx: L: lists the options in force");
	offhook_up(&fx_rfc);
	r = ca_cmd("CRCX", EP1, "C: A1\r\nM: recvonly\r\nL: p:30, a:PCMU\r\n");
	conn_id_of(r, id, sizeof(id));
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\nL: s:on\r\n", id);
	ca_cmd("MDCX", EP1, rest);
	snprintf(rest, sizeof(rest), "I: %s\r\nF: L\r\n", id);
	CHECK(has(ca_cmd("AUCX", EP1, rest), "L: p:30, a:PCMU, s:on\r\n"));
	fixture_stop();

	CASE("aucx: a malformed C: gets 510, an absent one passes");
	offhook_up(&fx_rfc);
	call_up(id, sizeof(id));
	snprintf(rest, sizeof(rest), "C: XYZ\r\nI: %s\r\nF: M\r\n", id);
	CHECK_UINT(code_of(ca_cmd("AUCX", EP1, rest)), 510);
	snprintf(rest, sizeof(rest), "C: B2\r\nI: %s\r\nF: M\r\n", id);
	CHECK_UINT(code_of(ca_cmd("AUCX", EP1, rest)), 516);
	snprintf(rest, sizeof(rest), "I: %s\r\nF: M\r\n", id);
	CHECK_UINT(code_of(ca_cmd("AUCX", EP1, rest)), 200);
	fixture_stop();

	CASE("aucx: a response over the datagram gets 533");
	offhook_up(&fx_rfc);
	sip_buf_init(&b, big, sizeof(big));
	sip_buf_add(&b, "C: A1\r\nM: recvonly\r\nL: a:PCMU");
	for (i = 0; i < 90; i++)
		sip_buf_add(&b, ";PCMU");
	sip_buf_add(&b, "\r\n");
	CHECK(sip_buf_ok(&b));
	r = ca_cmd("CRCX", EP1, big);
	CHECK_UINT(code_of(r), 200);
	conn_id_of(r, id, sizeof(id));
	snprintf(rest, sizeof(rest), "I: %s\r\nF: L,L,L,L,L,L,L,L,L,LC\r\n",
		 id);
	CHECK_UINT(code_of(ca_cmd("AUCX", EP1, rest)), 533);
	fixture_stop();

	CASE("mdcx: a partial L: keeps s: and t:");
	offhook_up(&fx_rfc);
	r = ca_cmd("CRCX", EP1, "C: A1\r\nM: recvonly\r\nL: a:PCMU, s:on, "
		   "t:B8\r\n");
	conn_id_of(r, id, sizeof(id));
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\nM: sendrecv\r\n"
		 "L: p:30\r\n\r\n" SDP_ANSWER, id);
	CHECK_UINT(code_of(ca_cmd("MDCX", EP1, rest)), 200);
	CHECK(gw.ep[0].conn[0].loc.codec[0].vad);
	CHECK_UINT(gw.ep[0].conn[0].media.rtp_dscp, 0xB8 >> 2);
	fixture_stop();

	CASE("events: a qualifier on an unknown name gets 522, on T: 512");
	offhook_up(&fx_rfc);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 1\r\nR: D/zz@AB\r\n")),
		   522);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, "X: 2\r\nT: D/5@AB\r\n")),
		   512);
	fixture_stop();

	CASE("signals: an unquoted long caller name keeps its head");
	up(&fx_rfc);
	memset(name, 'B', 300);
	name[300] = '\0';
	snprintf(rest, sizeof(rest), "X: 4D0\r\nS: L/ci(10/01/09/30, 5551234, "
		 "%s)\r\n", name);
	CHECK_UINT(code_of(ca_cmd("RQNT", EP1, rest)), 200);
	CHECK(hop_last(PHF_CID_SEND)->cid.name[0] == 'B');
	CHECK_UINT(strlen(hop_last(PHF_CID_SEND)->cid.name),
		   sizeof(hop_last(PHF_CID_SEND)->cid.name) - 1);
	fixture_stop();

	CASE("dlcx: several connections with X: alone get 510");
	offhook_up(&fx_rfc);
	ca_cmd("CRCX", EP1, "C: A1\r\nM: inactive\r\n");
	CHECK_UINT(code_of(ca_cmd("DLCX", EP1, "X: E2\r\n")), 510);
	CHECK(gw.ep[0].conn[0].used);
	fixture_stop();

	CASE("restart: endpoints of two entities get one RSIP each");
	fixture_start(&fx_rfc);
	str_copy(gw.ep[1].entity, sizeof(gw.ep[1].entity),
		 "ca2@[192.0.2.2]:2727");
	mgcp_ep_start(&gw);
	fake_advance(10001);
	rec = sent_to("RSIP", "192.0.2.1");
	CHECK(rec && has(rec->buf, " " EP1 " "));
	rec = sent_to("RSIP", "192.0.2.2");
	CHECK(rec && has(rec->buf, " " EP2 " "));
	CHECK(!has(sent("RSIP"), "aaln/*"));
	fixture_stop();

	CASE("disconnected: ncs puts the RSIP in front of an audit response");
	offhook_up(&fx_ncs);
	ca_cmd("RQNT", EP1, "X: B7\r\nR: L/hf(N), L/hu(N)\r\n");
	ev_hook(0, HAL_HOOK_FLASH);
	/* NCS 7.4.2: the NTFY ends at 2 * T-HIST */
	fake_advance(61000);
	CHECK(mgcp_txn_disc_active(&mgr));
	r = ca_cmd("AUEP", EP1, "F: RM\r\n");
	CHECK(!strncmp(r, "RSIP ", 5) && has(r, "\r\n.\r\n200 "));
	fixture_stop();

	CASE("disconnected: a 521 with N: to an endpoint's own RSIP moves it");
	offhook_up(&fx_rfc);
	ca_cmd("RQNT", EP1, "X: C2\r\nR: L/hf(N), L/hu(N)\r\n");
	ca_cmd("RQNT", EP2, "X: C3\r\nN: ca2@[192.0.2.2]:2727\r\n"
	       "R: L/hd(N)\r\n");
	ev_hook(0, HAL_HOOK_FLASH);
	ev_hook(1, HAL_HOOK_SEIZE);
	fake_advance(30000);
	mark_set();
	fake_advance(15000);
	rec = sent_to("RSIP", "192.0.2.2");
	CHECK(rec && has(rec->buf, " " EP2 " "));
	mark_set();
	ca_answer(rec ? rec->buf : "", 521, "N: ca3@192.0.2.1:2727\r\n");
	CHECK(!strcmp(gw.ep[1].entity, "ca3@192.0.2.1:2727"));
	rec = sent_to("RSIP", "192.0.2.1");
	CHECK(rec && has(rec->buf, " " EP2 " "));
	fixture_stop();
}

/* ------------------------------------------ call records and the call trace */

static void cdr_move_tests(void)
{
	char id[MGCP_ID_MAX], id2[MGCP_ID_MAX], rest[1024];
	const char *r;

	CASE("cdr: the record moves to the connection that stays, and a new "
	     "call on the freed stream keeps its own record");
	offhook_up(&fx_rfc);
	r = ca_cmd("CRCX", EP1, "C: A1\r\nM: recvonly\r\n");
	conn_id_of(r, id, sizeof(id));
	r = ca_cmd("CRCX", EP1, "C: A1\r\nM: recvonly\r\n");
	conn_id_of(r, id2, sizeof(id2));
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\n", id);
	CHECK_UINT(code_of(ca_cmd("DLCX", EP1, rest)), 250);
	CHECK(!strcmp(hop_streams(PHF_CDR_CALL_STARTED), "0 1"));
	CHECK(!strcmp(hop_streams(PHF_CDR_CALL_ENDED), "0"));
	CHECK(hop_last(PHF_CDR_CALL_ENDED)->on);
	CHECK_UINT(hop_last(PHF_CDR_CALL_CORRELATE)->stream, 1);
	CHECK_UINT(hop_last(PHF_CDR_CALL_CORRELATE)->val, 0);
	r = ca_cmd("CRCX", EP1, "C: B1\r\nM: recvonly\r\n");
	conn_id_of(r, id, sizeof(id));
	CHECK(!strcmp(gw.ep[0].conn[0].call_id, "B1"));
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\n", id2);
	CHECK_UINT(code_of(ca_cmd("DLCX", EP1, rest)), 250);
	CHECK(!strcmp(hop_streams(PHF_CDR_CALL_ENDED), "0 1"));
	CHECK(!hop_last(PHF_CDR_CALL_ENDED)->on);
	snprintf(rest, sizeof(rest), "C: B1\r\nI: %s\r\n", id);
	CHECK_UINT(code_of(ca_cmd("DLCX", EP1, rest)), 250);
	CHECK(!strcmp(hop_streams(PHF_CDR_CALL_STARTED), "0 1 0"));
	CHECK(!strcmp(hop_streams(PHF_CDR_CALL_ENDED), "0 1 0"));
	fixture_stop();

	CASE("cdr: a moved record keeps the answer of its call");
	offhook_up(&fx_rfc);
	call_up(id, sizeof(id));
	ca_cmd("CRCX", EP1, "C: A1\r\nM: recvonly\r\n");
	CHECK_UINT(phf.n[PHF_CDR_CALL_ANSWERED], 1);
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\n", id);
	CHECK_UINT(code_of(ca_cmd("DLCX", EP1, rest)), 250);
	CHECK_UINT(phf.n[PHF_CDR_CALL_ANSWERED], 2);
	CHECK_UINT(hop_last(PHF_CDR_CALL_ANSWERED)->stream, 1);
	CHECK(phf.cdr_ended.answered && phf.cdr_ended.transferred);
	fixture_stop();

	CASE("cdr: a line that leaves service closes one record per call");
	offhook_up(&fx_rfc);
	ca_cmd("CRCX", EP1, "C: A1\r\nM: recvonly\r\n");
	ca_cmd("CRCX", EP1, "C: A1\r\nM: recvonly\r\n");
	mgcp_ep_service(&gw, 0, false, MGCP_EP_REASON_OOS);
	CHECK_UINT(phf.n[PHF_CDR_CALL_STARTED], 1);
	CHECK_UINT(phf.n[PHF_CDR_CALL_ENDED], 1);
	CHECK(!phf.cdr_ended.transferred);
	fixture_stop();
}

static void cdr_tests(void)
{
	char id[MGCP_ID_MAX], id2[MGCP_ID_MAX], rest[1024];
	const char *r;

	CASE("cdr: a call opens at its first CRCX and ends at its last DLCX");
	offhook_up(&fx_rfc);
	r = ca_cmd("CRCX", EP1, "C: A1\r\nM: recvonly\r\nL: a:PCMU\r\n");
	conn_id_of(r, id, sizeof(id));
	CHECK_UINT(phf.n[PHF_CDR_CALL_STARTED], 1);
	CHECK(!phf.cdr_inbound);
	CHECK_UINT(phf.n[PHF_CALLTRACE_CALL_STARTED], 1);
	r = ca_cmd("CRCX", EP1, "C: A1\r\nM: inactive\r\n");
	conn_id_of(r, id2, sizeof(id2));
	CHECK_UINT(phf.n[PHF_CDR_CALL_STARTED], 1);
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\nM: sendrecv\r\n\r\n"
		 SDP_ANSWER, id);
	CHECK_UINT(code_of(ca_cmd("MDCX", EP1, rest)), 200);
	CHECK_UINT(phf.n[PHF_CDR_CALL_ANSWERED], 1);
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\nM: sendrecv\r\n", id);
	ca_cmd("MDCX", EP1, rest);
	CHECK_UINT(phf.n[PHF_CDR_CALL_ANSWERED], 1);
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\n", id2);
	CHECK_UINT(code_of(ca_cmd("DLCX", EP1, rest)), 250);
	CHECK_UINT(phf.n[PHF_CDR_CALL_ENDED], 0);
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\n", id);
	CHECK_UINT(code_of(ca_cmd("DLCX", EP1, rest)), 250);
	CHECK_UINT(phf.n[PHF_CDR_CALL_ENDED], 1);
	CHECK_UINT(phf.stream, 0);
	CHECK(phf.cdr_ended.answered && phf.cdr_ended.media_ran);
	CHECK_UINT(phf.cdr_ended.codec, HAL_CODEC_PCMU);
	CHECK_UINT(phf.cdr_ended.ptime_ms, 20);
	CHECK_UINT(phf.cdr_ended.rtp_port, 16384);
	CHECK(!strcmp(phf.reason, "dlcx"));
	fixture_stop();

	CASE("cdr: the media numbers go first, then the record closes, then "
	     "the stream stops");
	offhook_up(&fx_rfc);
	call_up(id, sizeof(id));
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\n", id);
	CHECK_UINT(code_of(ca_cmd("DLCX", EP1, rest)), 250);
	CHECK_UINT(hop_count(PHF_MEDIA_STATS_SNAPSHOT), 1);
	CHECK(hop_index(PHF_MEDIA_STATS_SNAPSHOT) >= 0);
	CHECK(hop_index(PHF_MEDIA_STATS_SNAPSHOT) <
	      hop_index(PHF_CDR_CALL_ENDED));
	CHECK(hop_index(PHF_CDR_CALL_ENDED) < hop_index(PHF_MEDIA_STOP));
	CHECK_UINT(hop_last(PHF_MEDIA_STATS_SNAPSHOT)->stream, 0);
	CHECK(phf.cdr_ended.media_ran);
	fixture_stop();

	CASE("cdr: a call that ran no media takes no media numbers");
	offhook_up(&fx_rfc);
	r = ca_cmd("CRCX", EP1, "C: A1\r\nM: recvonly\r\n");
	conn_id_of(r, id, sizeof(id));
	snprintf(rest, sizeof(rest), "C: A1\r\nI: %s\r\n", id);
	CHECK_UINT(code_of(ca_cmd("DLCX", EP1, rest)), 250);
	CHECK_UINT(phf.n[PHF_CDR_CALL_ENDED], 1);
	CHECK_UINT(phf.n[PHF_MEDIA_STATS_SNAPSHOT], 0);
	fixture_stop();

	CASE("cdr: a CRCX on an on-hook line opens an inbound record");
	up(&fx_rfc);
	ca_cmd("CRCX", EP1, "C: A1\r\nM: recvonly\r\n");
	CHECK_UINT(phf.n[PHF_CDR_CALL_STARTED], 1);
	CHECK(phf.cdr_inbound);
	mgcp_ep_service(&gw, 0, false, MGCP_EP_REASON_OOS);
	CHECK_UINT(phf.n[PHF_CDR_CALL_ENDED], 1);
	CHECK(!phf.cdr_ended.answered);
	fixture_stop();

	CASE("call trace: the state moves, the commands and the NTFY");
	up(&fx_rfc);
	CHECK(!strcmp(phf.before, "restarting") && !strcmp(phf.after, "idle"));
	ca_cmd("RQNT", EP1, "X: 70\r\nR: L/hd(N)\r\n");
	CHECK(!strcmp(phf.reason, "rqnt"));
	ev_hook(0, HAL_HOOK_SEIZE);
	CHECK(!strcmp(phf.before, "idle") && !strcmp(phf.after, "offhook"));
	CHECK(!strcmp(phf.reason, "ntfy"));
	fixture_stop();
}

int main(void)
{
	sig_tests();
	restart_tests();
	outbound_tests();
	conn_tests();
	inbound_tests();
	fax_tests();
	quarantine_tests();
	audit_tests();
	service_tests();
	disc_tests();
	rsip_tests();
	check_tests();
	option_tests();
	queue_tests();
	host_tests();
	record_tests();
	cdr_tests();
	cdr_move_tests();

	SUITE_CHECK_UINT(fake_timer_dropped, 0);

	return tu_summary("test_mgcp_ep");
}
