// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 */
#ifndef UVOICE_CFG_SCHEMA_H
#define UVOICE_CFG_SCHEMA_H

#include <limits.h>
#include <stddef.h>

enum cfg_type {
	CFG_BOOL,
	CFG_INT,
	CFG_STRING,
	CFG_ENUM
};

enum cfg_apply_class {
	APPLY_LIVE,
	APPLY_REREG,
	APPLY_RESTART_LINE,
	APPLY_RESTART_ALL,
	/* the reload refuses the change and the init script restarts the
	 * daemon (main.c uvoice_reload) */
	APPLY_RESTART_DAEMON
};

enum cfg_support {
	SUP_SUPPORTED,
	SUP_ACCEPTED_UNSUPPORTED,
	SUP_ALIAS
};

struct cfg_option_desc {
	const char *name;
	enum cfg_type type;
	const char *def;
	int min;
	int max;
	const char *const *enum_vals;
	enum cfg_apply_class apply_class;
	enum cfg_support support;
	const char *alias_of;
};

static const char *const cfg_bool_vals[] = { "0", "1", NULL };
static const char *const cfg_voip_model_vals[] = { "1", "2", NULL };
static const char *const cfg_transport_vals[] = { "udp", "tcp", "tls", NULL };
static const char *const cfg_protocol_vals[] = { "sip", "mgcp", NULL };
/* enum cfg_mgcp_dialect, cfg_mgcp_quarantine and cfg_mgcp_reply order */
static const char *const cfg_mgcp_dialect_vals[] = { "rfc3435", "ncs", NULL };
static const char *const cfg_mgcp_quarantine_vals[] = { "step", "loop", NULL };
static const char *const cfg_mgcp_reply_vals[] = {
	"source", "mgc_port", NULL
};
static const char *const cfg_tdm_backend_vals[] = {
	"alsa", "alsa-raw", "file", NULL
};
static const char *const cfg_sample_rate_vals[] = { "8000", "16000", NULL };
static const char *const cfg_restrict_source_vals[] = { "0", "1", "2", NULL };
static const char *const cfg_digit_esc_vals[] = { "0", "1", "2", NULL };
static const char *const cfg_local_conf_vals[] = { "0", "1", "2", NULL };
static const char *const cfg_call_fwd_mode_vals[] = { "0", "1", "2", "4", "6", NULL };
static const char *const cfg_dtmf_mode_vals[] = {
	"inband", "rfc2833", "sipinfo", "sip_info", "info", NULL
};
static const char *const cfg_flash_relay_vals[] = {
	"off", "rfc2833", "info", NULL
};
/* enum cfg_fwd_disc_battery order, so the table reads as the enum does */
static const char *const cfg_fwd_disc_battery_vals[] = {
	"none", "remove", "reverse", NULL
};
/* enum cfg_dns_srv_mode order, so the table reads as the enum does */
static const char *const cfg_dns_srv_mode_vals[] = {
	"auto", "always", "never", NULL
};
static const char *const cfg_session_refresher_vals[] = {
	"uac", "uas", "unspecified", NULL
};
static const char *const cfg_ptime_vals[] = { "10", "20", "30", "40", NULL };
static const char *const cfg_g711a_ptype_vals[] = { "8", "129", NULL };
static const char *const cfg_g729a_ptype_vals[] = { "18", "129", NULL };
static const char *const cfg_g722_ptype_vals[] = { "9", "129", NULL };
static const char *const cfg_opus_ptype_vals[] = {
	"96", "97", "98", "99", "100", "102", "103", "104",
	"105", "106", "107", "108", "109", "110", "111", "112", "113",
	"114", "115", "116", "117", "118", "119", "120", "121", "122",
	"123", "124", "125", "126", "127", "129", NULL
};
static const char *const cfg_g726_ptype_vals[] = {
	"1", "2", "9", "64", "65", "97", "98", "99", "100", "101", "102",
	"103", "104", "105", "106", "107", "108", "109", "110", "111",
	"112", "113", "114", "115", "116", "117", "118", "119", "120",
	"121", "122", "123", "124", "125", "126", "127", "129", NULL
};
static const char *const cfg_tr104_id_vals[] = {
	"1", "2", "3", "4", "5", "6", NULL
};

#define CFG_DESC(option_name, option_type, option_def, option_min, option_max, \
		 option_enum, option_apply, option_support, option_alias) \
	{ option_name, option_type, option_def, option_min, option_max, option_enum, \
	  option_apply, option_support, option_alias }

static const struct cfg_option_desc cfg_base_options[] = {
	CFG_DESC("enabled", CFG_BOOL, "0", 0, 1, cfg_bool_vals, APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	CFG_DESC("voip_model", CFG_ENUM, "1", 1, 2, cfg_voip_model_vals, APPLY_RESTART_ALL, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("channels", CFG_INT, NULL, 0, 2, NULL, APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	CFG_DESC("local_interface", CFG_STRING, "wan", 0, 0, NULL, APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	CFG_DESC("preset_log_level", CFG_INT, "0", 0, 4, NULL, APPLY_LIVE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("procd_log_enabled", CFG_BOOL, "0", 0, 1, cfg_bool_vals, APPLY_RESTART_ALL, SUP_ACCEPTED_UNSUPPORTED, NULL),
	/* Free-form: region_iso3_get() (line/line.c) maps 32 alpha-2 codes and
	 * passes every name of three characters or more to the backend
	 * upper-cased, so no closed list describes this row. It is the one
	 * regional input the config carries: every regional value comes from
	 * the backend's region data through the read-back. */
	CFG_DESC("region", CFG_STRING, "US", 0, 0, NULL, APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	CFG_DESC("supported_regions", CFG_STRING, NULL, 0, 0, NULL, APPLY_LIVE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("alert_info_prefix", CFG_STRING, "Bellcore-dr", 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("mwi_subscribe", CFG_BOOL, "0", 0, 1, cfg_bool_vals, APPLY_REREG, SUP_SUPPORTED, NULL),
	/* Whether the star code defaults of the channel table below are local.
	 * 0 hands them to the switch: a code loads only when the config names
	 * it, so the dialled digits reach the dial plan. A code the config
	 * names is local in either mode, and the literal "none" clears one. */
	CFG_DESC("star_codes_local", CFG_BOOL, "0", 0, 1, cfg_bool_vals, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("restrict_source", CFG_ENUM, "2", 0, 2, cfg_restrict_source_vals, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("options_interval", CFG_INT, "0", 0, INT_MAX, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("active_source_filter_enabled", CFG_BOOL, "0", 0, 1, cfg_bool_vals, APPLY_LIVE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("transport", CFG_ENUM, "udp", 0, 0, cfg_transport_vals, APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	CFG_DESC("protocol", CFG_ENUM, "sip", 0, 0, cfg_protocol_vals,
		 APPLY_RESTART_DAEMON, SUP_SUPPORTED, NULL),
	CFG_DESC("srtp_enabled", CFG_BOOL, "0", 0, 1, cfg_bool_vals, APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	CFG_DESC("keepalive_enabled", CFG_BOOL, "1", 0, 1, cfg_bool_vals, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("keepalive_interval", CFG_INT, "30", 1, INT_MAX, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("dns_cache_update_mode", CFG_BOOL, "1", 0, 1, cfg_bool_vals, APPLY_REREG, SUP_SUPPORTED, NULL),
	CFG_DESC("dns_move_unregister", CFG_BOOL, "1", 0, 1, cfg_bool_vals, APPLY_REREG, SUP_SUPPORTED, NULL),
	CFG_DESC("dns_srv_mode", CFG_ENUM, "auto", 0, 0, cfg_dns_srv_mode_vals, APPLY_REREG, SUP_SUPPORTED, NULL),
	CFG_DESC("dns_linger", CFG_INT, "3600", 0, 86400, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("rtcp_enabled", CFG_BOOL, "1", 0, 1, cfg_bool_vals, APPLY_RESTART_LINE, SUP_SUPPORTED, NULL),
	CFG_DESC("rtcp_interval", CFG_INT, NULL, 1, INT_MAX, NULL, APPLY_RESTART_LINE, SUP_SUPPORTED, NULL),
	CFG_DESC("ipv6_enabled", CFG_BOOL, "0", 0, 1, cfg_bool_vals, APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	CFG_DESC("tls_cert_path", CFG_STRING, NULL, 0, 0, NULL, APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	CFG_DESC("tls_cert_username", CFG_STRING, NULL, 0, 0, NULL, APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	CFG_DESC("tls_cert_password", CFG_STRING, NULL, 0, 0, NULL, APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	CFG_DESC("tls_server_verification_enabled", CFG_BOOL, "0", 0, 1, cfg_bool_vals, APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	CFG_DESC("tls_keylog_enabled", CFG_BOOL, "0", 0, 1, cfg_bool_vals, APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	{ NULL, CFG_STRING, NULL, 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL }
};

static const struct cfg_option_desc cfg_hal_options[] = {
	CFG_DESC("tdm_backend", CFG_ENUM, "alsa-raw", 0, 0, cfg_tdm_backend_vals, APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	CFG_DESC("tdm_playback", CFG_STRING, "plughw:0,0", 0, 0, NULL, APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	CFG_DESC("tdm_capture", CFG_STRING, "plughw:0,1", 0, 0, NULL, APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	CFG_DESC("slic_plugin", CFG_STRING, "/usr/lib/udsp/udsp-slic-proslic-mt798x.so", 0, 0, NULL, APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	CFG_DESC("fax_plugin", CFG_STRING, "", 0, 0, NULL, APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	CFG_DESC("spidev", CFG_STRING, "/dev/spidev0.0", 0, 0, NULL, APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	CFG_DESC("gpio_reset", CFG_STRING, "slic_reset", 0, 0, NULL, APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	CFG_DESC("gpio_interrupt", CFG_STRING, "slic_interrupt", 0, 0, NULL, APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	CFG_DESC("populated_ports", CFG_INT, "0", 0, 8, NULL, APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	CFG_DESC("channels", CFG_INT, "2", 1, 2, NULL, APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	/* Every period is 10 ms of standing playback latency; 3 matches the
	 * udsp default (udsp.h prefill_periods). */
	CFG_DESC("prefill_periods", CFG_INT, "3", 1, 16, NULL, APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	CFG_DESC("engine_cpu", CFG_INT, "3", -1, 255, NULL, APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	CFG_DESC("engine_prio", CFG_INT, "50", 0, 99, NULL, APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	/* Keep this ceiling with udsp-slic-proslic glue/plugin.c, which rejects
	 * an offset at or above PROSLIC_PCM_SLOT_BITS (16). */
	CFG_DESC("pcm_offset_bits", CFG_INT, "0", 0, 15, NULL, APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	CFG_DESC("sample_rate", CFG_ENUM, "8000", 8000, 16000, cfg_sample_rate_vals, APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	{ NULL, CFG_STRING, NULL, 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL }
};

/*
 * The debug section. One option, and it is a list of trace domain names
 * rather than a switch per domain: the domain set grows with the daemon and a
 * schema row per domain would need a config migration each time. APPLY_LIVE,
 * because a reload is the whole apply: trace.c rewrites its uci mask from
 * this string and nothing restarts.
 */
static const struct cfg_option_desc cfg_debug_options[] = {
	CFG_DESC("trace", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	{ NULL, CFG_STRING, NULL, 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL }
};

/*
 * The mgcp section. Every row is APPLY_RESTART_ALL. The low bound of max2
 * is max1 + 1, and that of rto_max_ms and t_hist_s is the option before it;
 * the rows carry the lowest value that bound can take. The rto_init_ms
 * floor is the 100 ms RTO floor of the MGCP transaction layer.
 */
static const struct cfg_option_desc cfg_mgcp_options[] = {
	CFG_DESC("call_agent", CFG_STRING, "", 0, 0, NULL,
		 APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	CFG_DESC("call_agent_port", CFG_INT, "2727", 1, 65535, NULL,
		 APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	CFG_DESC("local_port", CFG_INT, "2427", 1, 65535, NULL,
		 APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	CFG_DESC("domain", CFG_STRING, "", 0, 0, NULL,
		 APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	CFG_DESC("bracketed_ip", CFG_BOOL, "0", 0, 1, cfg_bool_vals,
		 APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	CFG_DESC("endpoint_pattern", CFG_STRING, "aaln/{line}@{host}", 0, 0,
		 NULL, APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	CFG_DESC("line_base", CFG_INT, "1", 0, 255, NULL,
		 APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	CFG_DESC("dialect", CFG_ENUM, "rfc3435", 0, 0, cfg_mgcp_dialect_vals,
		 APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	CFG_DESC("max1", CFG_INT, "5", 1, 20, NULL,
		 APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	CFG_DESC("max2", CFG_INT, "7", 2, 30, NULL,
		 APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	CFG_DESC("rto_init_ms", CFG_INT, "200", 100, 10000, NULL,
		 APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	CFG_DESC("rto_max_ms", CFG_INT, "4000", 100, 60000, NULL,
		 APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	CFG_DESC("t_max_s", CFG_INT, "20", 1, 300, NULL,
		 APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	CFG_DESC("t_hist_s", CFG_INT, "30", 1, 3600, NULL,
		 APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	CFG_DESC("persistent_events", CFG_STRING, "hd,hu,hf", 0, 0, NULL,
		 APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	CFG_DESC("quarantine_default", CFG_ENUM, "step", 0, 0,
		 cfg_mgcp_quarantine_vals, APPLY_RESTART_ALL, SUP_SUPPORTED,
		 NULL),
	CFG_DESC("send_ptime", CFG_BOOL, "1", 0, 1, cfg_bool_vals,
		 APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	CFG_DESC("reply_method", CFG_ENUM, "source", 0, 0, cfg_mgcp_reply_vals,
		 APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	CFG_DESC("signaling_dscp", CFG_INT, "24", 0, 63, NULL,
		 APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	CFG_DESC("restart_delay_max_s", CFG_INT, "10", 0, 600, NULL,
		 APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	{ NULL, CFG_STRING, NULL, 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL }
};

static const struct cfg_option_desc cfg_channel_options[] = {
	CFG_DESC("id", CFG_INT, NULL, 0, 1, NULL, APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	CFG_DESC("ux_id", CFG_INT, NULL, 1, 2, NULL, APPLY_RESTART_ALL, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("enabled", CFG_BOOL, "0", 0, 1, cfg_bool_vals, APPLY_RESTART_LINE, SUP_SUPPORTED, NULL),
	CFG_DESC("sip_username", CFG_STRING, "", 0, 0, NULL, APPLY_REREG, SUP_SUPPORTED, NULL),
	CFG_DESC("sip_auth_username", CFG_STRING, "", 0, 0, NULL, APPLY_REREG, SUP_SUPPORTED, NULL),
	CFG_DESC("sip_callerid_name", CFG_STRING, "", 0, 0, NULL, APPLY_REREG, SUP_SUPPORTED, NULL),
	CFG_DESC("sip_secret", CFG_STRING, "", 0, 0, NULL, APPLY_REREG, SUP_SUPPORTED, NULL),
	CFG_DESC("sip_proxy", CFG_STRING, "", 0, 0, NULL, APPLY_REREG, SUP_SUPPORTED, NULL),
	CFG_DESC("sip_port", CFG_INT, "5060", 1, 65535, NULL, APPLY_REREG, SUP_SUPPORTED, NULL),
	CFG_DESC("sip_outbound_proxy", CFG_STRING, NULL, 0, 0, NULL, APPLY_REREG, SUP_SUPPORTED, NULL),
	CFG_DESC("sip_outbound_proxy_port", CFG_INT, NULL, 1, 65535, NULL, APPLY_REREG, SUP_SUPPORTED, NULL),
	CFG_DESC("sip_domain", CFG_STRING, "", 0, 0, NULL, APPLY_REREG, SUP_SUPPORTED, NULL),
	CFG_DESC("sip_registrar", CFG_STRING, NULL, 0, 0, NULL, APPLY_REREG, SUP_SUPPORTED, NULL),
	CFG_DESC("sip_registrar_port", CFG_INT, NULL, 1, 65535, NULL, APPLY_REREG, SUP_SUPPORTED, NULL),
	CFG_DESC("sip_realm", CFG_STRING, "", 0, 0, NULL, APPLY_REREG, SUP_SUPPORTED, NULL),
	CFG_DESC("sip_registration_expiry", CFG_INT, "3600", 0, INT_MAX, NULL, APPLY_REREG, SUP_SUPPORTED, NULL),
	CFG_DESC("sip_registration_exp_time", CFG_INT, "3600", 0, INT_MAX, NULL, APPLY_REREG, SUP_ALIAS, "sip_registration_expiry"),
	/* Proxy-switch retry after DNS TTL change, not authentication backoff. */
	CFG_DESC("sip_register_retry", CFG_INT, "5", 0, INT_MAX, NULL, APPLY_REREG, SUP_SUPPORTED, NULL),
	CFG_DESC("sip_rereg_delta_exp_time", CFG_INT, "0", 0, INT_MAX, NULL, APPLY_REREG, SUP_ALIAS, "sip_register_retry"),
	CFG_DESC("sip_local_port", CFG_INT, "5060", 1, 65535, NULL, APPLY_RESTART_LINE, SUP_SUPPORTED, NULL),
	CFG_DESC("sip_tos", CFG_INT, "104", 0, 255, NULL, APPLY_RESTART_LINE, SUP_SUPPORTED, NULL),
	CFG_DESC("rtp_tos", CFG_INT, "184", 0, 255, NULL, APPLY_RESTART_LINE, SUP_SUPPORTED, NULL),
	CFG_DESC("rtcp_tos", CFG_INT, "184", 0, 255, NULL, APPLY_RESTART_LINE, SUP_SUPPORTED, NULL),
	CFG_DESC("rtp_local_port_min", CFG_INT, NULL, 1024, 65535, NULL, APPLY_RESTART_LINE, SUP_SUPPORTED, NULL),
	CFG_DESC("rtp_local_port_max", CFG_INT, NULL, 1024, 65535, NULL, APPLY_RESTART_LINE, SUP_SUPPORTED, NULL),
	CFG_DESC("primary_dns", CFG_STRING, "0.0.0.0", 0, 0, NULL, APPLY_REREG, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("secondary_dns", CFG_STRING, "0.0.0.0", 0, 0, NULL, APPLY_REREG, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("local_interface", CFG_STRING, NULL, 0, 0, NULL, APPLY_RESTART_ALL, SUP_ALIAS, "base.local_interface"),
	/* accepted and ignored, and not an alias: the account reads transport
	 * from the base section alone, so nothing normalizes this row into that
	 * one */
	CFG_DESC("transport", CFG_ENUM, NULL, 0, 0, cfg_transport_vals, APPLY_RESTART_ALL, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("options_interval", CFG_INT, NULL, 0, INT_MAX, NULL, APPLY_LIVE, SUP_ALIAS, "base.options_interval"),
	CFG_DESC("sip_dtmf_mode", CFG_ENUM, "inband", 0, 0, cfg_dtmf_mode_vals, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("sip_flash_relay", CFG_ENUM, "info", 0, 0, cfg_flash_relay_vals, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("sip_session_max_expires", CFG_INT, NULL, 1, INT_MAX, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("sip_session_min_interval", CFG_INT, NULL, 1, INT_MAX, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("sip_session_refresher", CFG_ENUM, "uac", 0, 0, cfg_session_refresher_vals, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("sip_timer_t1", CFG_INT, "500", 1, INT_MAX, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("sip_timer_t2", CFG_INT, "4000", 1, INT_MAX, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("sip_timer_t4", CFG_INT, "5000", 1, INT_MAX, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("sip_timer_a", CFG_INT, "500", 1, INT_MAX, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("sip_timer_b", CFG_INT, "32000", 1, INT_MAX, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("sip_timer_c", CFG_INT, NULL, 180001, INT_MAX, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("sip_timer_d", CFG_INT, "32000", 1, INT_MAX, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("sip_timer_e", CFG_INT, "500", 1, INT_MAX, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("sip_timer_f", CFG_INT, "32000", 1, INT_MAX, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("sip_timer_g", CFG_INT, "500", 1, INT_MAX, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("sip_timer_h", CFG_INT, "32000", 1, INT_MAX, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("sip_timer_i", CFG_INT, "5000", 1, INT_MAX, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("sip_timer_j", CFG_INT, "32000", 1, INT_MAX, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("sip_timer_k", CFG_INT, "5000", 1, INT_MAX, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	/* The option of the UAS side, read at the start of a leg. With 1, the
	 * 180 of an INVITE that carries Require: 100rel or Supported: 100rel
	 * goes out reliably (RFC 3262 3). With 0 every 180 is plain, including
	 * the answer to a Require: 100rel, which deviates from the MUST of
	 * RFC 3262 3. The UAC side reads nothing here: it
	 * acknowledges every reliable 1xx it receives. */
	CFG_DESC("prack", CFG_BOOL, "0", 0, 1, cfg_bool_vals, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("early_media_enabled", CFG_BOOL, "1", 0, 1, cfg_bool_vals, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("rtcp_cname", CFG_STRING, "", 0, 0, NULL, APPLY_RESTART_LINE, SUP_SUPPORTED, NULL),
	/* The leg's T.38 gate, read once at the leg start together with the
	 * backend HAL_FEAT_T38 bit. G.711 fax passthrough and the passthrough
	 * switch of the answer tone run whatever it holds. A leg that is
	 * already up keeps the value it started with, which is what makes it
	 * RESTART_LINE. */
	CFG_DESC("fax_t38_enable", CFG_BOOL, "0", 0, 1, cfg_bool_vals, APPLY_RESTART_LINE, SUP_SUPPORTED, NULL),
	/* On by default: without it, a call the far end already converted
	 * runs its page over the voice path, with the canceller and the
	 * adaptive jitter buffer on the carrier. The leg reads it at its own
	 * start, the fax_t38_enable rule. */
	CFG_DESC("fax_upspeed_follow", CFG_BOOL, "1", 0, 1, cfg_bool_vals, APPLY_RESTART_LINE, SUP_SUPPORTED, NULL),
	CFG_DESC("default_packetization_rate", CFG_ENUM, "20", 10, 40, cfg_ptime_vals, APPLY_RESTART_LINE, SUP_SUPPORTED, NULL),
	CFG_DESC("cn_enable", CFG_BOOL, "0", 0, 1, cfg_bool_vals, APPLY_RESTART_LINE, SUP_SUPPORTED, NULL),
	CFG_DESC("g711a_ptype", CFG_ENUM, "8", 0, 129, cfg_g711a_ptype_vals, APPLY_RESTART_LINE, SUP_SUPPORTED, NULL),
	CFG_DESC("g729a_ptype", CFG_ENUM, "18", 0, 129, cfg_g729a_ptype_vals, APPLY_RESTART_LINE, SUP_SUPPORTED, NULL),
	CFG_DESC("g726_ptype", CFG_ENUM, "111", 0, 129, cfg_g726_ptype_vals, APPLY_RESTART_LINE, SUP_SUPPORTED, NULL),
	CFG_DESC("g722_ptype", CFG_ENUM, "9", 0, 129, cfg_g722_ptype_vals, APPLY_RESTART_LINE, SUP_SUPPORTED, NULL),
	CFG_DESC("opus_ptype", CFG_ENUM, "96", 0, 129, cfg_opus_ptype_vals, APPLY_RESTART_LINE, SUP_SUPPORTED, NULL),
	CFG_DESC("narrow_band_align_rate", CFG_STRING, NULL, 0, 0, NULL, APPLY_RESTART_LINE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("narrow_band_align_ptype", CFG_INT, NULL, 0, 129, NULL, APPLY_RESTART_LINE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("narrow_band_efficient_rate", CFG_STRING, NULL, 0, 0, NULL, APPLY_RESTART_LINE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("narrow_band_efficient_ptype", CFG_INT, "103", 0, 129, NULL, APPLY_RESTART_LINE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("wide_band_align_rate", CFG_STRING, NULL, 0, 0, NULL, APPLY_RESTART_LINE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("wide_band_align_ptype", CFG_INT, "104", 0, 129, NULL, APPLY_RESTART_LINE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("wide_band_efficient_rate", CFG_STRING, NULL, 0, 0, NULL, APPLY_RESTART_LINE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("wide_band_efficient_ptype", CFG_INT, "105", 0, 129, NULL, APPLY_RESTART_LINE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("digit_map_base64", CFG_STRING, "Knh4LlR8eC5U", 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("digit_esc_enabled", CFG_ENUM, "1", 0, 2, cfg_digit_esc_vals, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("start_digit_timer", CFG_INT, "16", 0, INT_MAX, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("interdigit_timeout", CFG_INT, "4", 0, INT_MAX, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("interdigit_timeout_long", CFG_INT, "16", 0, INT_MAX, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("interdigit_long", CFG_INT, "16", 0, INT_MAX, NULL, APPLY_LIVE, SUP_ALIAS, "interdigit_timeout_long"),
	CFG_DESC("roh_timer_sec", CFG_INT, "60", 0, INT_MAX, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("busy_tone_timer_sec", CFG_INT, "40", 0, INT_MAX, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("flash_service_timer_sec", CFG_INT, "5", 0, INT_MAX, NULL, APPLY_LIVE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("dial_plan_format", CFG_INT, "2", 0, INT_MAX, NULL, APPLY_LIVE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("voicemail_exp_time", CFG_INT, "3600", 0, INT_MAX, NULL, APPLY_LIVE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("call_fwd_local", CFG_BOOL, "1", 0, 1, cfg_bool_vals, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("call_fwd_local_mode", CFG_ENUM, "0", 0, 6, cfg_call_fwd_mode_vals, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("call_fwd_uncond_uri", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("call_fwd_busy_uri", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("call_fwd_no_answer_uri", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("call_fwd_no_answer_timeout", CFG_INT, "15", 0, INT_MAX, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	/* The star code defaults, here and below, apply under the base
	 * star_codes_local 1 alone; under 0 an absent code loads empty. */
	CFG_DESC("call_fwd_uncond_exten", CFG_STRING, "*21#", 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("call_fwd_uncond_cancel_exten", CFG_STRING, "#21#", 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("call_fwd_uncond_query_exten", CFG_STRING, "*#21#", 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("call_fwd_busy_exten", CFG_STRING, "*67#", 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("call_fwd_busy_cancel_exten", CFG_STRING, "#67#", 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("call_fwd_busy_query_exten", CFG_STRING, "*#67#", 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("call_fwd_no_answer_exten", CFG_STRING, "*61#", 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("call_fwd_no_answer_cancel_exten", CFG_STRING, "#61#", 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("call_fwd_no_answer_query_exten", CFG_STRING, "*#61#", 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("call_fwd_unreachable_exten", CFG_STRING, "*62#", 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("call_fwd_unreachable_cancel_exten", CFG_STRING, "#62#", 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("call_fwd_all_exten", CFG_STRING, "*004#", 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	/* Two options that carry one string cancel each other in the feature
	 * table, so neither would run. The cancel code therefore takes the
	 * '#'-leading form its three siblings use and not the "*004#" of the
	 * enable code. */
	CFG_DESC("call_fwd_all_cancel_exten", CFG_STRING, "#004#", 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("call_waiting_local", CFG_BOOL, NULL, 0, 1, cfg_bool_vals, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("call_waiting_enabled", CFG_BOOL, "1", 0, 1, cfg_bool_vals, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("call_waiting_enable_exten", CFG_STRING, "*43#", 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("call_waiting_disable_exten", CFG_STRING, "#43#", 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("call_waiting_query_exten", CFG_STRING, "*#43#", 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("cancel_cw_on_exten", CFG_STRING, NULL, 0, 0, NULL, APPLY_LIVE, SUP_ALIAS, "call_waiting_disable_exten"),
	CFG_DESC("cancel_cw_off_exten", CFG_STRING, NULL, 0, 0, NULL, APPLY_LIVE, SUP_ALIAS, "call_waiting_enable_exten"),
	CFG_DESC("cid_remote_enable", CFG_BOOL, "1", 0, 1, cfg_bool_vals, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("cid_outgoing_enable", CFG_BOOL, "1", 0, 1, cfg_bool_vals, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("cid_delivery_local", CFG_BOOL, NULL, 0, 1, cfg_bool_vals, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("cid_single_call_enable_exten", CFG_STRING, "*31#", 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("cid_single_call_disable_exten", CFG_STRING, "#31#", 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	/* Unseeded, because the obvious seed is the "*31#"/"#31#" of the
	 * one-call pair, and two options with one string cancel each other in
	 * the feature table. */
	CFG_DESC("cid_enable_exten", CFG_STRING, NULL, 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("cid_disable_exten", CFG_STRING, NULL, 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	/* The three-digit transfer codes. The three rows are supported; the
	 * FSM runs the transfer the star code table only matches. */
	CFG_DESC("blind_transfer_exten", CFG_STRING, "*072", 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("attended_transfer_exten", CFG_STRING, "*073", 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("consult_transfer_exten", CFG_STRING, "*074", 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("do_not_disturb_enabled", CFG_BOOL, "0", 0, 1, cfg_bool_vals, APPLY_LIVE, SUP_SUPPORTED, NULL),
	/* *78 with *79 is the NANP do not disturb pair, so a local-mode
	 * subscriber dials the code the switch would take for the same
	 * service. *57 stays free, because it is the NANP customer originated
	 * trace code. #78 keeps this table's '#'-leading off form. */
	CFG_DESC("do_not_disturb_on_exten", CFG_STRING, "*78", 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("do_not_disturb_off_exten", CFG_STRING, "#78", 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("local_conf_enabled", CFG_ENUM, "1", 0, 2, cfg_local_conf_vals, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("warmline_enable", CFG_BOOL, "0", 0, 1, cfg_bool_vals, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("warmline_number", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("warmline_timeout", CFG_INT, "8", 0, INT_MAX, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	/* The uint policy of this table accepts any value, and line/line.c
	 * clamps what a timer can hold. */
	CFG_DESC("forward_disconnect_delay", CFG_INT, "0", 0, INT_MAX, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("forward_disconnect_battery", CFG_ENUM, "none", 0, 0, cfg_fwd_disc_battery_vals, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("amwi", CFG_BOOL, "1", 0, 1, cfg_bool_vals, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("vmwi", CFG_BOOL, "1", 0, 1, cfg_bool_vals, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("lec_control", CFG_BOOL, NULL, 0, 1, cfg_bool_vals, APPLY_RESTART_LINE, SUP_SUPPORTED, NULL),
	/* The default 1 is a behaviour: 0 disables the canceller. lec_control 1
	 * with no lec_enable keeps the canceller on, so a change to 0 here
	 * would disable it for every config that takes manual control without
	 * naming a value. */
	CFG_DESC("lec_enable", CFG_BOOL, "1", 0, 1, cfg_bool_vals, APPLY_RESTART_LINE, SUP_SUPPORTED, NULL),
	CFG_DESC("lec_nlp", CFG_BOOL, NULL, 0, 1, cfg_bool_vals, APPLY_RESTART_LINE, SUP_SUPPORTED, NULL),
	/* The answer-tone disabler of the DSP canceller. Both defaults are the
	 * G.168 clause 7 behaviour, so a config that names neither gets it:
	 * release the disabler on a quiet holding band, and take only the
	 * phase-reversed tone to the full disable. */
	CFG_DESC("lec_tone_latch", CFG_BOOL, "0", 0, 1, cfg_bool_vals, APPLY_RESTART_LINE, SUP_SUPPORTED, NULL),
	CFG_DESC("lec_tone_force_disable", CFG_BOOL, "0", 0, 1, cfg_bool_vals, APPLY_RESTART_LINE, SUP_SUPPORTED, NULL),
	/* SLIC side hardware gains. The cs pair is accepted and unused. The ps
	 * pair is whole dB in 0..-20 and reaches the SLIC through the udsp
	 * backend; the DSP gains of the half decibel loss plan below are not
	 * a substitute. An out-of-range value clamps with a warning, the
	 * uvoice-wide convention. */
	CFG_DESC("cs_tx_gain", CFG_INT, NULL, INT_MIN, INT_MAX, NULL, APPLY_RESTART_LINE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("cs_rx_gain", CFG_INT, NULL, INT_MIN, INT_MAX, NULL, APPLY_RESTART_LINE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("ps_tx_gain", CFG_INT, "-3", -20, 0, NULL, APPLY_RESTART_LINE, SUP_SUPPORTED, NULL),
	CFG_DESC("ps_rx_gain", CFG_INT, "-9", -20, 0, NULL, APPLY_RESTART_LINE, SUP_SUPPORTED, NULL),
	CFG_DESC("half_db_rx_loss", CFG_BOOL, "0", 0, 1, cfg_bool_vals, APPLY_RESTART_LINE, SUP_SUPPORTED, NULL),
	CFG_DESC("half_db_tx_loss", CFG_BOOL, "0", 0, 1, cfg_bool_vals, APPLY_RESTART_LINE, SUP_SUPPORTED, NULL),
	CFG_DESC("announcement_type", CFG_INT, "4", 0, INT_MAX, NULL, APPLY_RESTART_LINE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("dtmf_digit_levels", CFG_INT, "32768", 0, 65535, NULL, APPLY_RESTART_LINE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	/* The udsp jitter-buffer floor for the first 2 s of a call and the
	 * park point of every prefill, quantized to whole packet spans: 50
	 * parks 60 ms of queue at ptime 20, 30 parks 40 ms. */
	CFG_DESC("jitter_target", CFG_INT, "30", 0, 65535, NULL, APPLY_RESTART_LINE, SUP_SUPPORTED, NULL),
	CFG_DESC("jitter_buffer_max", CFG_INT, "250", 0, 65535, NULL, APPLY_RESTART_LINE, SUP_SUPPORTED, NULL),
	CFG_DESC("speed_dial_num_0", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("speed_dial_num_1", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("speed_dial_num_2", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("speed_dial_num_3", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("speed_dial_num_4", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("speed_dial_num_5", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("speed_dial_num_6", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("speed_dial_num_7", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("speed_dial_num_8", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("speed_dial_num_9", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("block_number_enable", CFG_BOOL, "0", 0, 1, cfg_bool_vals, APPLY_LIVE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("block_number_0", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("block_number_1", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("block_number_2", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("block_number_3", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("block_number_4", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("block_number_5", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("block_number_6", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("block_number_7", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("block_number_8", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("block_number_9", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("allow_number_enable", CFG_BOOL, "0", 0, 1, cfg_bool_vals, APPLY_LIVE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("allow_number_0", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("allow_number_1", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("allow_number_2", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("allow_number_3", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("allow_number_4", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("allow_number_5", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("allow_number_6", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("allow_number_7", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("allow_number_8", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("allow_number_9", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("distinctive_ring_enable", CFG_BOOL, "0", 0, 1, cfg_bool_vals, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("distinctive_ring_0", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("distinctive_ring_1", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("distinctive_ring_2", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("distinctive_ring_3", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("distinctive_ring_4", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("distinctive_ring_5", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("distinctive_ring_6", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("distinctive_ring_7", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("distinctive_ring_8", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("distinctive_ring_9", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("distinctive_ring_def_0", CFG_INT, NULL, 0, 7, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("distinctive_ring_def_1", CFG_INT, NULL, 0, 7, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("distinctive_ring_def_2", CFG_INT, NULL, 0, 7, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("distinctive_ring_def_3", CFG_INT, NULL, 0, 7, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("distinctive_ring_def_4", CFG_INT, NULL, 0, 7, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("distinctive_ring_def_5", CFG_INT, NULL, 0, 7, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("distinctive_ring_def_6", CFG_INT, NULL, 0, 7, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("distinctive_ring_def_7", CFG_INT, NULL, 0, 7, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("distinctive_ring_def_8", CFG_INT, NULL, 0, 7, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	CFG_DESC("distinctive_ring_def_9", CFG_INT, NULL, 0, 7, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL),
	{ NULL, CFG_STRING, NULL, 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL }
};

static const struct cfg_option_desc cfg_codec_options[] = {
	CFG_DESC("tr104_name", CFG_STRING, NULL, 0, 0, NULL, APPLY_RESTART_LINE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("tr104_id", CFG_ENUM, NULL, 1, 6, cfg_tr104_id_vals, APPLY_RESTART_LINE, SUP_SUPPORTED, NULL),
	CFG_DESC("cwmp_alias", CFG_STRING, "", 0, 0, NULL, APPLY_LIVE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("bitrate", CFG_INT, NULL, 0, 512000, NULL, APPLY_RESTART_LINE, SUP_SUPPORTED, NULL),
	CFG_DESC("fec", CFG_BOOL, "1", 0, 1, cfg_bool_vals, APPLY_RESTART_LINE, SUP_SUPPORTED, NULL),
	CFG_DESC("complexity", CFG_INT, NULL, 0, 10, NULL, APPLY_RESTART_LINE, SUP_SUPPORTED, NULL),
	CFG_DESC("packetization", CFG_STRING, "10,20,30,40", 0, 0, NULL, APPLY_RESTART_LINE, SUP_ACCEPTED_UNSUPPORTED, NULL),
	CFG_DESC("silence_suppression", CFG_BOOL, "0", 0, 1, cfg_bool_vals, APPLY_RESTART_LINE, SUP_SUPPORTED, NULL),
	CFG_DESC("priority", CFG_INT, NULL, 1, INT_MAX, NULL, APPLY_RESTART_LINE, SUP_SUPPORTED, NULL),
	CFG_DESC("default_priority", CFG_INT, NULL, 1, INT_MAX, NULL, APPLY_RESTART_LINE, SUP_SUPPORTED, NULL),
	CFG_DESC("enabled", CFG_BOOL, "1", 0, 1, cfg_bool_vals, APPLY_RESTART_LINE, SUP_SUPPORTED, NULL),
	CFG_DESC("channel", CFG_INT, NULL, 1, 2, NULL, APPLY_RESTART_ALL, SUP_SUPPORTED, NULL),
	{ NULL, CFG_STRING, NULL, 0, 0, NULL, APPLY_LIVE, SUP_SUPPORTED, NULL }
};

#undef CFG_DESC

#endif
