// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Loaded /etc/config/voip state.
 *
 * Scope. This carries the loaded fields. The descriptor set documents the
 * complete schema, including options that nothing consumes yet.
 *
 * Boundary. Every UCI detail lives in cfg/uci.c, which reads the options
 * through the cfg/schema.h descriptors so a default written once in the schema
 * is the default the loader applies. New loaded fields also appear in the
 * diagnostic dump unless their values are credentials.
 *
 * Lifetime. A struct cfg is immutable once loaded. Reload builds a new one and
 * swaps the pointer on the uloop thread, so live objects must copy the strings
 * they need instead of holding pointers into the current config.
 */
#ifndef UVOICE_CFG_H
#define UVOICE_CFG_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* The 2 channel cap of the deployed schema. */
#define CFG_CHANNEL_MAX		2
#define CFG_STR_MAX		64
/* tls_cert_path holds an FTP, HTTP or HTTPS URL, which outgrows CFG_STR_MAX */
#define CFG_TLS_PATH_MAX	128
#define CFG_HAL_PATH_MAX	128

/* The ceiling of a configured line timer in seconds, for both halves of the
 * line shell: uloop_timeout_set takes an int, and no subscriber timer runs
 * for a day. */
#define LINE_TIMER_MAX_S	86400u
#define CFG_HAL_DEV_MAX		64
#define CFG_HAL_GPIO_MAX	32
/* The trace domain list of voip.debug; trace.c parses at the same width. */
#define CFG_DEBUG_TRACE_MAX	128
#define CFG_CODEC_MAX		16
#define CFG_DIGIT_MAP_MAX	256
#define CFG_DISTINCTIVE_RING_MAX 10
/* voip.base.protocol, the longest word with its terminator */
#define CFG_PROTOCOL_MAX	16
/* voip.mgcp.dialect, the longest word with its terminator */
#define CFG_MGCP_DIALECT_MAX	8
#define CFG_MGCP_DOMAIN_MAX	128

struct blob_buf;

/*
 * The SIP transport timers the transaction layer needs, in milliseconds. These
 * map one for one onto struct sip_timer_cfg; the account copies them across and
 * lets sip_timer_cfg_fixup() derive any that a config left at zero. Timer C is
 * an INVITE timer and belongs to the call layer, so it is not here.
 */
struct cfg_sip_timers {
	unsigned int t1;
	unsigned int t2;
	unsigned int t4;
	unsigned int a;
	unsigned int b;
	unsigned int d;
	unsigned int e;
	unsigned int f;
	unsigned int g;
	unsigned int h;
	unsigned int i;
	unsigned int j;
	unsigned int k;
};

struct cfg_codec {
	bool enabled;
	bool silence_suppression;
	unsigned int tr104_id;
	unsigned int priority;
	unsigned int default_priority;
	unsigned int channel;		/* 1-based, 0 selects all; cfg_channel.id is 0-based */
	unsigned int bitrate;
	bool fec;
	unsigned int complexity;
};

enum cfg_dtmf_mode {
	CFG_DTMF_INBAND = 0,
	CFG_DTMF_RFC2833,
	CFG_DTMF_INFO,
};

enum cfg_refresher {
	CFG_REFRESH_UAC = 0,
	CFG_REFRESH_UAS,
	CFG_REFRESH_UNSPEC,
};

enum cfg_flash_relay {
	CFG_FLASH_RELAY_OFF = 0,
	CFG_FLASH_RELAY_RFC2833,
	CFG_FLASH_RELAY_INFO,
};

/*
 * What a far-end clear does to the loop. NONE is zero, so a config that names
 * neither forward disconnect option leaves the feature off.
 *
 * REVERSE reverses the battery for the delay. REMOVE breaks the loop for the
 * delay (line/line.c fwd_disc_remove), which needs a backend that can take the
 * line down; line/line.c reports a fault on one that cannot.
 */
enum cfg_fwd_disc_battery {
	CFG_FWD_DISC_BATT_NONE = 0,
	CFG_FWD_DISC_BATT_REMOVE,
	CFG_FWD_DISC_BATT_REVERSE,
};

/*
 * Which DNS question a line asks for its next hop. AUTO is zero, so a config
 * that names the option not at all follows RFC 3263 4.2: a port configured for
 * the next hop asks A and AAAA of the name, and no port asks SRV with the
 * address fallback behind it. A port-named name with no address then gets an
 * SRV question too. ALWAYS asks SRV whatever port the config names. NEVER
 * asks the address alone, for a zone whose SRV answers are wrong.
 */
enum cfg_dns_srv_mode {
	CFG_DNS_SRV_AUTO = 0,
	CFG_DNS_SRV_ALWAYS,
	CFG_DNS_SRV_NEVER,
};

/*
 * The signaling protocol the daemon runs (voip.base.protocol). SIP is zero,
 * so a config that does not name the option runs SIP. Whether the image
 * carries the protocol is a separate question, which the protocol registry
 * answers (proto/proto.h proto_select).
 */
enum cfg_protocol {
	CFG_PROTO_SIP = 0,
	CFG_PROTO_MGCP,
};

/*
 * The MGCP text the gateway speaks (voip.mgcp.dialect): RFC 3435 or the
 * PacketCable NCS profile. CFG_MGCP_RFC3435 is zero, so a config that does
 * not name the option speaks RFC 3435.
 */
enum cfg_mgcp_dialect {
	CFG_MGCP_RFC3435 = 0,
	CFG_MGCP_NCS,
};

/* The quarantine handling of a request that carries no Q: (RFC 3435 4.4.1) */
enum cfg_mgcp_quarantine {
	CFG_MGCP_QUARANTINE_STEP = 0,
	CFG_MGCP_QUARANTINE_LOOP,
};

/*
 * Where a response goes: SOURCE to the source address of the command (RFC
 * 3435 3.5), MGC_PORT to the call agent port (call_agent_port).
 */
enum cfg_mgcp_reply {
	CFG_MGCP_REPLY_SOURCE = 0,
	CFG_MGCP_REPLY_MGC_PORT,
};

struct cfg_hal {
	char tdm_backend[16];
	char tdm_playback[CFG_HAL_DEV_MAX];
	char tdm_capture[CFG_HAL_DEV_MAX];
	char slic_plugin[CFG_HAL_PATH_MAX];
	char fax_plugin[CFG_HAL_PATH_MAX];
	char spidev[CFG_HAL_DEV_MAX];
	char gpio_reset[CFG_HAL_GPIO_MAX];
	char gpio_interrupt[CFG_HAL_GPIO_MAX];
	unsigned int populated_ports_cfg;
	unsigned int populated_ports;
	unsigned int channels;
	unsigned int prefill_periods;
	int engine_cpu;
	int engine_prio;
	unsigned int pcm_offset_bits;
	unsigned int sample_rate;
	int handset_mic_gain;
	int headset_mic_gain;
	int speaker_mic_gain;
	int handset_out_gain;
	int headset_out_gain;
	unsigned int ring_sound;
	unsigned int ring_volume;
};

/* These array widths mirror udsp/src/include/udsp/udsp.h struct udsp_config. */
_Static_assert(sizeof(((struct cfg_hal *)0)->tdm_backend) == 16,
	       "HAL backend width must match udsp_config");
_Static_assert(sizeof(((struct cfg_hal *)0)->tdm_playback) == 64,
	       "HAL device width must match udsp_config");
_Static_assert(sizeof(((struct cfg_hal *)0)->slic_plugin) == 128,
	       "HAL path width must match udsp_config");
_Static_assert(sizeof(((struct cfg_hal *)0)->gpio_reset) == 32,
	       "HAL GPIO width must match udsp_config");

/*
 * A port stored as 0 is unset and inherits another option through the schema
 * fallback chain, which the account resolves: an empty sip_outbound_proxy_port
 * or sip_registrar_port takes sip_port. sip_port and sip_local_port carry the
 * schema default 5060, so they are never 0.
 */
struct cfg_channel {
	bool present;
	bool enabled;
	unsigned int id;		/* voip.channelN.id, 0 based line index */

	char sip_username[CFG_STR_MAX];
	char sip_auth_username[CFG_STR_MAX];
	char sip_callerid_name[CFG_STR_MAX];
	char sip_secret[CFG_STR_MAX];
	char sip_proxy[CFG_STR_MAX];
	char sip_outbound_proxy[CFG_STR_MAX];
	char sip_domain[CFG_STR_MAX];
	char sip_registrar[CFG_STR_MAX];
	char sip_realm[CFG_STR_MAX];
	char transport[CFG_STR_MAX];		/* channel alias, empty takes base */
	char local_interface[CFG_STR_MAX];	/* channel alias, empty takes base */

	unsigned int sip_port;
	/* Presence, not value: the schema default 5060 always loads, so this
	 * flag is the only place an explicitly named remote port is
	 * distinguishable from the default one. */
	bool sip_port_set;
	unsigned int sip_outbound_proxy_port;	/* 0 inherits sip_port */
	unsigned int sip_registrar_port;	/* 0 inherits sip_port */
	unsigned int sip_registration_expiry;
	unsigned int sip_register_retry;
	unsigned int sip_local_port;
	unsigned int sip_tos;
	unsigned int options_interval;		/* channel alias, 0 takes base */

	enum cfg_dtmf_mode dtmf_mode;
	enum cfg_flash_relay sip_flash_relay;
	unsigned int session_max_expires;
	unsigned int session_min_interval;
	enum cfg_refresher session_refresher;
	unsigned int timer_c_ms;
	char digit_map[CFG_DIGIT_MAP_MAX];
	unsigned int digit_esc;
	unsigned int start_digit_s;
	unsigned int interdigit_s;
	unsigned int interdigit_long_s;
	unsigned int roh_s;
	unsigned int busy_tone_s;
	unsigned int flash_svc_s;
	bool call_waiting_enabled;
	bool call_waiting_local;
	bool call_waiting_local_set;
	char cw_enable_code[CFG_STR_MAX];
	char cw_disable_code[CFG_STR_MAX];
	char cw_query_code[CFG_STR_MAX];
	bool cid_remote_enable;
	bool cid_outgoing_enable;
	bool cid_delivery_local;
	bool cid_delivery_local_set;
	char cid_enable_exten[CFG_STR_MAX];
	char cid_disable_exten[CFG_STR_MAX];
	char cid_single_call_enable_exten[CFG_STR_MAX];
	char cid_single_call_disable_exten[CFG_STR_MAX];
	bool call_fwd_local;
	unsigned int call_fwd_local_mode;
	char call_fwd_uncond_uri[CFG_STR_MAX];
	char call_fwd_busy_uri[CFG_STR_MAX];
	char call_fwd_no_answer_uri[CFG_STR_MAX];
	unsigned int call_fwd_no_answer_timeout;
	char call_fwd_uncond_exten[CFG_STR_MAX];
	char call_fwd_uncond_cancel_exten[CFG_STR_MAX];
	char call_fwd_uncond_query_exten[CFG_STR_MAX];
	char call_fwd_busy_exten[CFG_STR_MAX];
	char call_fwd_busy_cancel_exten[CFG_STR_MAX];
	char call_fwd_busy_query_exten[CFG_STR_MAX];
	char call_fwd_no_answer_exten[CFG_STR_MAX];
	char call_fwd_no_answer_cancel_exten[CFG_STR_MAX];
	char call_fwd_no_answer_query_exten[CFG_STR_MAX];
	char call_fwd_unreachable_exten[CFG_STR_MAX];
	char call_fwd_unreachable_cancel_exten[CFG_STR_MAX];
	char call_fwd_all_exten[CFG_STR_MAX];
	char call_fwd_all_cancel_exten[CFG_STR_MAX];
	/*
	 * The three transfer codes. The option names are shifted one slot
	 * against the transfers they configure: attended_transfer_exten is the
	 * unattended transfer and consult_transfer_exten the consultative,
	 * attended one. call.h and features.h carry the whole table.
	 */
	char blind_transfer_exten[CFG_STR_MAX];
	char attended_transfer_exten[CFG_STR_MAX];
	char consult_transfer_exten[CFG_STR_MAX];
	bool do_not_disturb_enabled;
	char do_not_disturb_on_exten[CFG_STR_MAX];
	char do_not_disturb_off_exten[CFG_STR_MAX];
	bool distinctive_ring_enable;
	char distinctive_ring[CFG_DISTINCTIVE_RING_MAX][CFG_STR_MAX];
	unsigned int distinctive_ring_def[CFG_DISTINCTIVE_RING_MAX];
	bool distinctive_ring_def_set[CFG_DISTINCTIVE_RING_MAX];
	bool warmline_enable;
	char warmline_number[CFG_STR_MAX];
	unsigned int warmline_timeout_s;
	/*
	 * Forward disconnect, as a pair. The delay is the length of the
	 * reversal or of the loop break in milliseconds and 0 turns the
	 * feature off; the battery form says what the clear does to the loop.
	 * Neither option enables anything alone (cfg_fwd_disc_on).
	 */
	unsigned int forward_disconnect_delay_ms;
	enum cfg_fwd_disc_battery forward_disconnect_battery;
	/*
	 * The two message waiting presentations, one gate each: amwi is the
	 * stutter dial tone, vmwi the FSK record toward the display.
	 */
	bool amwi;
	bool vmwi;
	/*
	 * The line echo canceller: lec_control 0 ignores the two
	 * values below and the daemon policy stands, lec_control 1 takes manual
	 * control of the canceller.
	 *
	 * lec_nlp carries a presence flag because an absent option follows the
	 * policy value while an explicit 0 turns the nonlinear processor off.
	 * lec_enable needs none: its schema default is 1, so it always loads a
	 * value.
	 *
	 * The two tone options are uvoice's own and stand outside lec_control:
	 * the answer-tone disabler of the DSP runs under the daemon policy too,
	 * and these only pick its release rule and its trigger mapping. Both
	 * default to 0, which is what G.168 clause 7 asks for, so neither needs
	 * a presence flag.
	 */
	bool lec_control;
	bool lec_control_set;
	bool lec_enable;
	bool lec_nlp;
	bool lec_nlp_set;
	bool lec_tone_latch;
	bool lec_tone_force_disable;
	/*
	 * The SLIC analog pair, whole dB in -20..0. line/line.c doubles them
	 * into the half-dB HAL pair; the DSP loss plan below is a separate
	 * plane and never mixes with them.
	 */
	int ps_tx_gain;
	int ps_rx_gain;
	/*
	 * The 0.5 dB loss plan (AU/NZ). Each option asks for one DSP gain step
	 * of attenuation in one direction; line/line.c owns the reversal onto
	 * the two channel gains.
	 */
	bool half_db_rx_loss;
	bool half_db_tx_loss;
	unsigned int local_conf_enabled;	/* 0 off, 1 conference, 2 conference with transfer */
	unsigned int ptime_ms;
	bool cn_enable;
	unsigned int g711a_ptype;
	unsigned int g729a_ptype;
	unsigned int g726_ptype;
	bool fax_t38_enable;
	/*
	 * A recent remote answer tone qualifies a G.711-only re-INVITE as a
	 * far-end upspeed. This option governs the follow profile, codec clamp,
	 * and fallback codec entry in the leg's list.
	 * The remote tone arms the kind regardless of this option.
	 * The calling side still offers no T.38.
	 */
	bool fax_upspeed_follow;
	bool prack;
	bool early_media_enabled;
	unsigned int jitter_target;
	unsigned int jitter_max;
	unsigned int rtp_tos;
	unsigned int rtcp_tos;
	unsigned int rtp_port_min;
	unsigned int rtp_port_max;
	char rtcp_cname[CFG_STR_MAX];

	struct cfg_sip_timers timers;
	unsigned int g722_ptype;
	unsigned int opus_ptype;
};

/*
 * Whether the channel asks for the forward disconnect: a nonzero delay and a
 * battery form other than none. The core and the protocol each read it from
 * their own copy of the channel.
 */
static inline bool cfg_fwd_disc_on(const struct cfg_channel *ch)
{
	return ch->forward_disconnect_delay_ms &&
	       ch->forward_disconnect_battery != CFG_FWD_DISC_BATT_NONE;
}

/*
 * The canceller pair the channel puts in effect, alone, because several
 * readers need it and a second copy of the resolution would drift: line/line.c
 * pushes it to the DSP and reports the pair that push carried
 * (line_lec_view_get), and the MGCP endpoint checks the e: of an L:
 * against it.
 *
 * The daemon policy: the canceller and its nonlinear processor are both on.
 */
static inline void cfg_channel_lec_effective(const struct cfg_channel *ch,
					     bool *ec_enable, bool *nlp_enable)
{
	*ec_enable = true;
	*nlp_enable = true;

	/* lec_control 0 ignores both values below, so the policy above
	 * stands */
	if (!ch->lec_control)
		return;

	*ec_enable = ch->lec_enable;
	/* and an absent lec_nlp keeps the policy value: only a value the
	 * config carries reaches the canceller */
	if (ch->lec_nlp_set)
		*nlp_enable = ch->lec_nlp;
}

/*
 * voip.debug. The comma list of trace domains the config
 * asks for; trace.c owns the vocabulary and warns about a name it does not
 * know. It is no credential, so cfg_dump() reports it.
 */
struct cfg_debug {
	char trace[CFG_DEBUG_TRACE_MAX];
};

/*
 * voip.mgcp. The loader reads it under every protocol word, because it is
 * core and a build can carry no MGCP unit; an absent section loads the
 * defaults. An empty call_agent loads.
 *
 * The six transaction values are the RFC 3435 thresholds and timers, with
 * the RFC values as defaults. The loader keeps max2 above max1, rto_max_ms
 * at or above rto_init_ms and t_hist_s at or above t_max_s.
 */
struct cfg_mgcp {
	char call_agent[CFG_STR_MAX];
	unsigned int call_agent_port;
	unsigned int local_port;
	char domain[CFG_MGCP_DOMAIN_MAX];	/* endpoint host part */
	bool bracketed_ip;
	char endpoint_pattern[CFG_STR_MAX];	/* {line} and {host} */
	unsigned int line_base;
	char dialect[CFG_MGCP_DIALECT_MAX];	/* cfg_mgcp_dialect_of() */
	unsigned int max1;
	unsigned int max2;
	unsigned int rto_init_ms;
	unsigned int rto_max_ms;
	unsigned int t_max_s;
	unsigned int t_hist_s;
	char persistent_events[CFG_STR_MAX];	/* "none" loads empty */
	enum cfg_mgcp_quarantine quarantine_default;
	bool send_ptime;
	enum cfg_mgcp_reply reply_method;
	unsigned int signaling_dscp;
	unsigned int restart_delay_max_s;
};

struct cfg {
	bool loaded;			/* false when /etc/config/voip is absent */
	bool enabled;			/* voip.base.enabled */
	unsigned int channels;		/* voip.base.channels, clamped */

	char local_interface[CFG_STR_MAX];	/* default wan */
	char transport[CFG_STR_MAX];		/* default udp */
	/*
	 * The signaling protocol word, "sip" or "mgcp". The default is sip. A
	 * change needs a daemon restart: the reload refuses it, and the init
	 * script then restarts the daemon. cfg_protocol_of() maps the word.
	 */
	char protocol[CFG_PROTOCOL_MAX];
	char ui[CFG_PROTOCOL_MAX];
	/*
	 * The SDES SRTP switch. The call layer reads it at leg start together
	 * with the backend HAL_FEAT_SRTP bit, so a backend without SRTP still
	 * runs plain RTP under a config that asks for it. It is no credential,
	 * so cfg_dump() reports it.
	 */
	bool srtp_enabled;
	char region[CFG_STR_MAX];		/* default US */
	unsigned int restrict_source;		/* 0 open, 1 proxy IPs, 2 pinhole */
	unsigned int options_interval;
	unsigned int keepalive_interval;
	bool keepalive_enabled;
	bool dns_cache_update_mode;		/* 1 re-register on a changed DNS answer */
	/*
	 * 1 removes the binding at the server a DNS move left, with a REGISTER
	 * of Expires 0 sent once the new server granted one. 0 lets it lapse.
	 */
	bool dns_move_unregister;
	/*
	 * Which question a line asks for its next hop. The account derives the
	 * kind per line from this word and the ports the line names.
	 */
	enum cfg_dns_srv_mode dns_srv_mode;
	/*
	 * Seconds a proxy address a resolver answer named keeps its inbound
	 * admission after it leaves the answer: the keepalive round holds its
	 * conntrack pinhole under restrict_source 2, and mode 1 keeps its
	 * allow rule. 0 turns the retention off. A pool that publishes one
	 * rotating record per query needs it, because inbound requests can
	 * arrive from any pool member.
	 */
	unsigned int dns_linger;
	bool mwi_subscribe;
	/* 0 loads a star code only when the config names it (schema.h). */
	bool star_codes_local;
	bool rtcp_enabled;
	/*
	 * The IPv6 operator gate. 0, the default, keeps the
	 * stack IPv4: the resolver asks A alone and every socket binds
	 * AF_INET. 1 lets the resolver ask AAAA. It lets a line take the
	 * family of its current registrar target. It lets IPv6 media reach a
	 * backend that sets HAL_FEAT_IPV6_MEDIA. uvoice reads it and never
	 * writes it.
	 */
	bool ipv6_enabled;
	unsigned int rtcp_interval;
	char alert_info_prefix[CFG_STR_MAX];

	/*
	 * The five base TLS options. All five join the account base-restart
	 * set, the two booleans shape the TLS contexts account.c loads, and
	 * the fetch options reach uvoice_tls_cert.sh
	 * through event_tls_cert_start() (event.h). tls_cert_path names the
	 * archive that helper fetches, and the two credentials go to that
	 * fetch and nowhere else, so cfg_dump() must keep them out of the
	 * diagnostic dump under the credential rule of this header.
	 */
	char tls_cert_path[CFG_TLS_PATH_MAX];
	char tls_cert_username[CFG_STR_MAX];
	char tls_cert_password[CFG_STR_MAX];
	bool tls_server_verification_enabled;
	bool tls_keylog_enabled;

	struct cfg_hal hal;
	struct cfg_debug debug;
	struct cfg_mgcp mgcp;
	struct cfg_codec codec[CFG_CODEC_MAX];
	unsigned int codecs;

	struct cfg_channel channel[CFG_CHANNEL_MAX];
};

/* Never returns NULL: a missing or broken package yields a disabled config so
 * the daemon stays up and reports the fault over ubus. */
struct cfg *cfg_load(void);
void cfg_free(struct cfg *cfg);
void cfg_dump(const struct cfg *cfg, struct blob_buf *b);

/*
 * The protocol cfg->protocol names. An empty word answers CFG_PROTO_SIP, the
 * default. The loader replaces a word it does not know with the default, so
 * every loaded config names one of the two values.
 */
enum cfg_protocol cfg_protocol_of(const struct cfg *cfg);
bool cfg_ui_desk(const struct cfg *cfg);

/**
 * cfg_ring_sound_names() - the words of voip.hal.ring_sound
 *
 * Return: a NULL terminated array, in the order of the ring_sound index.
 */
const char *const *cfg_ring_sound_names(void);

/**
 * cfg_hal_range() - the range of an integer option of voip.hal
 * @name: the option name
 * @min: filled with the lowest value
 * @max: filled with the highest value
 *
 * Return: 0, or -ENOENT if voip.hal has no option @name.
 */
int cfg_hal_range(const char *name, int *min, int *max);

/*
 * The dialect cfg->mgcp.dialect names. An empty word answers
 * CFG_MGCP_RFC3435, the default. The loader replaces a word it does not know
 * with the default, so every loaded config names one of the two values. A
 * static inline, because the MGCP unit reads it and links no cfg object.
 */
static inline enum cfg_mgcp_dialect cfg_mgcp_dialect_of(const struct cfg *cfg)
{
	if (!strcmp(cfg->mgcp.dialect, "ncs"))
		return CFG_MGCP_NCS;

	return CFG_MGCP_RFC3435;
}

#endif
