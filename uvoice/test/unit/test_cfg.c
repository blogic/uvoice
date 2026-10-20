// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libubox/blobmsg.h>
#include <libubox/ulog.h>
#include <libubox/utils.h>

#include "cfg/cfg.h"
#include "cfg/schema.h"
#include "test_util.h"
#include "test_warn.h"
#include "trace.h"

#define CHECK_CSTR(got, want) CHECK(!strcmp((got), (want)))

/*
 * A fault path leaves a warning behind, and "warns once" is part of the
 * rule: a fault that warns twice is as wrong as one that warns not at all. The
 * loader keeps no counter to read, and adding one to production code would
 * serve nothing but this suite, so the suite reads the warning through the
 * capture of test_warn.h.
 */
struct cfg_fixture {
	struct cfg *cfg;
	char dir[PATH_MAX];
	char path[PATH_MAX];
};

static struct cfg_fixture fixture_load(const char *text)
{
	struct cfg_fixture fixture = { 0 };
	const char *tmp_dir = getenv("TMPDIR");
	FILE *file;
	int written;

	if (!tmp_dir || !*tmp_dir)
		tmp_dir = "/tmp";
	written = snprintf(fixture.dir, sizeof(fixture.dir),
			   "%s/uvoice-cfg-XXXXXX", tmp_dir);
	if (written < 0 || (size_t)written >= sizeof(fixture.dir))
		return fixture;
	if (!mkdtemp(fixture.dir))
		return fixture;
	if (!text) {
		setenv("UVOICE_UCI_CONFDIR", fixture.dir, 1);
		fixture.cfg = cfg_load();
		return fixture;
	}

	written = snprintf(fixture.path, sizeof(fixture.path), "%s/voip", fixture.dir);
	if (written < 0 || (size_t)written >= sizeof(fixture.path)) {
		rmdir(fixture.dir);
		fixture.dir[0] = '\0';
		return fixture;
	}
	file = fopen(fixture.path, "w");
	if (!file) {
		rmdir(fixture.dir);
		fixture.dir[0] = '\0';
		return fixture;
	}

	fputs(text, file);
	fclose(file);
	setenv("UVOICE_UCI_CONFDIR", fixture.dir, 1);
	fixture.cfg = cfg_load();

	return fixture;
}

static void fixture_free(struct cfg_fixture *fixture)
{
	cfg_free(fixture->cfg);
	if (fixture->path[0])
		unlink(fixture->path);
	if (fixture->dir[0])
		rmdir(fixture->dir);
}

static bool fixture_ready(struct cfg_fixture *fixture)
{
	CHECK(fixture->cfg != NULL);
	if (fixture->cfg)
		return true;

	fixture_free(fixture);
	return false;
}

#define FIXTURE_LOAD_OR_RETURN(fixture, expression) \
	do { \
		(fixture) = (expression); \
		if (!fixture_ready(&(fixture))) \
			return; \
	} while (0)

static struct cfg_fixture channel_option_load(const char *option);
static struct cfg_fixture hal_option_load(const char *option);

static const char full_config[] =
	"config base 'base'\n"
	"\toption channels '1'\n"
	"\toption rtcp_enabled '0'\n"
	"\toption rtcp_interval '55'\n"
	"\toption alert_info_prefix 'Ring-X'\n"
	"config channel 'channel1'\n"
	"\toption id '0'\n"
	"\toption sip_dtmf_mode 'info'\n"
	"\toption sip_session_max_expires '7200'\n"
	"\toption sip_session_min_interval '120'\n"
	"\toption sip_session_refresher 'uas'\n"
	"\toption sip_timer_c '200000'\n"
	"\toption digit_map_base64 'MTJ4LlQ='\n"
	"\toption digit_esc_enabled '2'\n"
	"\toption start_digit_timer '11'\n"
	"\toption interdigit_timeout '6'\n"
	"\toption interdigit_timeout_long '19'\n"
	"\toption roh_timer_sec '70'\n"
	"\toption busy_tone_timer_sec '45'\n"
	"\toption flash_service_timer_sec '7'\n"
	"\toption call_waiting_enabled '0'\n"
	"\toption call_waiting_local '1'\n"
	"\toption call_waiting_enable_exten '*90#'\n"
	"\toption call_waiting_disable_exten '#90#'\n"
	"\toption call_waiting_query_exten '*#90#'\n"
	"\toption warmline_enable '1'\n"
	"\toption warmline_number '15551212'\n"
	"\toption warmline_timeout '12'\n"
	"\toption forward_disconnect_delay '900'\n"
	"\toption forward_disconnect_battery 'reverse'\n"
	"\toption amwi '0'\n"
	"\toption vmwi '1'\n"
	"\toption local_conf_enabled '0'\n"
	"\toption default_packetization_rate '30'\n"
	"\toption cn_enable '1'\n"
	"\toption g711a_ptype '129'\n"
	"\toption g729a_ptype '129'\n"
	"\toption g726_ptype '120'\n"
	"\toption g722_ptype '129'\n"
	"\toption opus_ptype '112'\n"
	"\toption fax_t38_enable '1'\n"
	/* the two channel bools this fixture turns OFF, because their schema
	 * default is 1: a loader that ignored either would still read true */
	"\toption fax_upspeed_follow '0'\n"
	"\toption early_media_enabled '0'\n"
	"\toption jitter_target '60'\n"
	"\toption jitter_buffer_max '110'\n"
	"\toption rtp_tos '100'\n"
	"\toption rtcp_tos '120'\n"
	"\toption rtp_local_port_min '30000'\n"
	"\toption rtp_local_port_max '30008'\n"
	"\toption rtcp_cname 'line-one'\n"
	"config codec 'full_codec'\n"
	"\toption enabled '0'\n"
	"\toption silence_suppression '1'\n"
	"\toption tr104_id '6'\n"
	"\toption bitrate '32000'\n"
	"\toption fec '0'\n"
	"\toption complexity '7'\n"
	"\toption priority '7'\n"
	"\toption default_priority '2'\n"
	"\toption channel '2'\n";

static const char m2_config[] =
	"config base 'base'\n"
	"\toption region 'GB'\n"
	"config channel 'channel1'\n"
	"\toption id '0'\n"
	"\toption sip_flash_relay 'off'\n"
	"\toption cid_remote_enable '0'\n"
	"\toption cid_outgoing_enable '0'\n"
	"\toption cid_delivery_local '1'\n"
	"\toption cid_enable_exten '*81#'\n"
	"\toption cid_disable_exten '#81#'\n"
	"\toption cid_single_call_enable_exten '*82#'\n"
	"\toption cid_single_call_disable_exten '#82#'\n"
	"\toption call_fwd_local '0'\n"
	"\toption call_fwd_local_mode '6'\n"
	"\toption call_fwd_uncond_uri 'sip:all@example.test'\n"
	"\toption call_fwd_busy_uri 'sip:busy@example.test'\n"
	"\toption call_fwd_no_answer_uri 'sip:late@example.test'\n"
	"\toption call_fwd_no_answer_timeout '22'\n"
	"\toption call_fwd_uncond_exten '*11#'\n"
	"\toption call_fwd_uncond_cancel_exten '#11#'\n"
	"\toption call_fwd_uncond_query_exten '*#11#'\n"
	"\toption call_fwd_busy_exten '*12#'\n"
	"\toption call_fwd_busy_cancel_exten '#12#'\n"
	"\toption call_fwd_busy_query_exten '*#12#'\n"
	"\toption call_fwd_no_answer_exten '*13#'\n"
	"\toption call_fwd_no_answer_cancel_exten '#13#'\n"
	"\toption call_fwd_no_answer_query_exten '*#13#'\n"
	"\toption call_fwd_unreachable_exten '*14#'\n"
	"\toption call_fwd_unreachable_cancel_exten '#14#'\n"
	"\toption call_fwd_all_exten '*15#'\n"
	"\toption call_fwd_all_cancel_exten '#15#'\n"
	"\toption do_not_disturb_enabled '1'\n"
	"\toption do_not_disturb_on_exten '*91#'\n"
	"\toption do_not_disturb_off_exten '#91#'\n"
	"\toption distinctive_ring_enable '1'\n"
	"\toption distinctive_ring_0 '12025550100'\n"
	"\toption distinctive_ring_9 '12025550109'\n"
	"\toption distinctive_ring_def_0 '3'\n"
	"\toption distinctive_ring_def_9 '6'\n";

static void explicit_tests(void)
{
	struct cfg_fixture fixture;
	const struct cfg_channel *ch;
	const struct cfg_codec *codec;

	CASE("full explicit base options");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(full_config));
	CHECK(!fixture.cfg->rtcp_enabled);
	CHECK_UINT(fixture.cfg->rtcp_interval, 55);
	CHECK_CSTR(fixture.cfg->alert_info_prefix, "Ring-X");

	ch = &fixture.cfg->channel[0];
	CASE("full explicit session options");
	CHECK(ch->dtmf_mode == CFG_DTMF_INFO);
	CHECK_UINT(ch->session_max_expires, 7200);
	CHECK_UINT(ch->session_min_interval, 120);
	CHECK(ch->session_refresher == CFG_REFRESH_UAS);
	CHECK_UINT(ch->timer_c_ms, 200000);

	CASE("full explicit dial map options");
	CHECK_CSTR(ch->digit_map, "12x.T");
	CHECK_UINT(ch->digit_esc, 2);
	CHECK_UINT(ch->start_digit_s, 11);
	CHECK_UINT(ch->interdigit_s, 6);
	CHECK_UINT(ch->interdigit_long_s, 19);

	CASE("full explicit dial timers");
	CHECK_UINT(ch->roh_s, 70);
	CHECK_UINT(ch->busy_tone_s, 45);
	CHECK_UINT(ch->flash_svc_s, 7);

	CASE("full explicit call waiting options");
	CHECK(!ch->call_waiting_enabled);
	CHECK(ch->call_waiting_local_set);
	CHECK(ch->call_waiting_local);
	CHECK_CSTR(ch->cw_enable_code, "*90#");
	CHECK_CSTR(ch->cw_disable_code, "#90#");
	CHECK_CSTR(ch->cw_query_code, "*#90#");

	CASE("full explicit warmline options");
	CHECK(ch->warmline_enable);
	CHECK_CSTR(ch->warmline_number, "15551212");
	CHECK_UINT(ch->warmline_timeout_s, 12);
	CHECK_UINT(ch->local_conf_enabled, 0);

	CASE("full explicit forward disconnect options");
	CHECK_UINT(ch->forward_disconnect_delay_ms, 900);
	CHECK(ch->forward_disconnect_battery == CFG_FWD_DISC_BATT_REVERSE);

	/* The two message waiting gates are independent bools here, never one
	 * composite value. */
	CASE("full explicit message waiting gates");
	CHECK(!ch->amwi);
	CHECK(ch->vmwi);

	CASE("full explicit codec media options");
	CHECK_UINT(ch->ptime_ms, 30);
	CHECK(ch->cn_enable);
	CHECK_UINT(ch->g711a_ptype, 129);
	CHECK_UINT(ch->g729a_ptype, 129);
	CHECK_UINT(ch->g726_ptype, 120);
	CHECK_UINT(ch->g722_ptype, 129);
	CHECK_UINT(ch->opus_ptype, 112);
	CHECK(ch->fax_t38_enable);
	CHECK(!ch->fax_upspeed_follow);
	CHECK(!ch->early_media_enabled);

	CASE("full explicit RTP options");
	CHECK_UINT(ch->jitter_target, 60);
	CHECK_UINT(ch->jitter_max, 110);
	CHECK_UINT(ch->rtp_tos, 100);
	CHECK_UINT(ch->rtcp_tos, 120);
	CHECK_UINT(ch->rtp_port_min, 30000);
	CHECK_UINT(ch->rtp_port_max, 30008);
	CHECK_CSTR(ch->rtcp_cname, "line-one");

	CASE("full explicit codec section");
	CHECK_UINT(fixture.cfg->codecs, 1);
	codec = &fixture.cfg->codec[0];
	CHECK(!codec->enabled);
	CHECK(codec->silence_suppression);
	CHECK_UINT(codec->tr104_id, 6);
	CHECK_UINT(codec->bitrate, 32000);
	CHECK(!codec->fec);
	CHECK_UINT(codec->complexity, 7);
	CHECK_UINT(codec->priority, 7);
	CHECK_UINT(codec->default_priority, 2);
	CHECK_UINT(codec->channel, 2);
	fixture_free(&fixture);
}

static void m2_explicit_tests(void)
{
	struct cfg_fixture fixture;
	const struct cfg_channel *ch;

	CASE("the named region loads as the selector");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(m2_config));
	CHECK_CSTR(fixture.cfg->region, "GB");

	ch = &fixture.cfg->channel[0];
	CASE("explicit flash relay and CID values load");
	CHECK(ch->sip_flash_relay == CFG_FLASH_RELAY_OFF);
	CHECK(!ch->cid_remote_enable);
	CHECK(!ch->cid_outgoing_enable);
	CHECK(ch->cid_delivery_local_set);
	CHECK(ch->cid_delivery_local);
	CHECK_CSTR(ch->cid_enable_exten, "*81#");
	CHECK_CSTR(ch->cid_disable_exten, "#81#");
	CHECK_CSTR(ch->cid_single_call_enable_exten, "*82#");
	CHECK_CSTR(ch->cid_single_call_disable_exten, "#82#");

	CASE("explicit forwarding values load");
	CHECK(!ch->call_fwd_local);
	CHECK_UINT(ch->call_fwd_local_mode, 6);
	CHECK_CSTR(ch->call_fwd_uncond_uri, "sip:all@example.test");
	CHECK_CSTR(ch->call_fwd_busy_uri, "sip:busy@example.test");
	CHECK_CSTR(ch->call_fwd_no_answer_uri, "sip:late@example.test");
	CHECK_UINT(ch->call_fwd_no_answer_timeout, 22);

	CASE("explicit forwarding extension values load");
	CHECK_CSTR(ch->call_fwd_uncond_exten, "*11#");
	CHECK_CSTR(ch->call_fwd_uncond_cancel_exten, "#11#");
	CHECK_CSTR(ch->call_fwd_uncond_query_exten, "*#11#");
	CHECK_CSTR(ch->call_fwd_busy_exten, "*12#");
	CHECK_CSTR(ch->call_fwd_busy_cancel_exten, "#12#");
	CHECK_CSTR(ch->call_fwd_busy_query_exten, "*#12#");
	CHECK_CSTR(ch->call_fwd_no_answer_exten, "*13#");
	CHECK_CSTR(ch->call_fwd_no_answer_cancel_exten, "#13#");
	CHECK_CSTR(ch->call_fwd_no_answer_query_exten, "*#13#");
	CHECK_CSTR(ch->call_fwd_unreachable_exten, "*14#");
	CHECK_CSTR(ch->call_fwd_unreachable_cancel_exten, "#14#");
	CHECK_CSTR(ch->call_fwd_all_exten, "*15#");
	CHECK_CSTR(ch->call_fwd_all_cancel_exten, "#15#");

	CASE("explicit DND and distinctive ring values load");
	CHECK(ch->do_not_disturb_enabled);
	CHECK_CSTR(ch->do_not_disturb_on_exten, "*91#");
	CHECK_CSTR(ch->do_not_disturb_off_exten, "#91#");
	CHECK(ch->distinctive_ring_enable);
	CHECK_CSTR(ch->distinctive_ring[0], "12025550100");
	CHECK_CSTR(ch->distinctive_ring[9], "12025550109");
	CHECK(ch->distinctive_ring_def_set[0]);
	CHECK(ch->distinctive_ring_def_set[9]);
	CHECK_UINT(ch->distinctive_ring_def[0], 3);
	CHECK_UINT(ch->distinctive_ring_def[9], 6);
	fixture_free(&fixture);
}

static void default_tests(void)
{
	/* star_codes_local 1: the star code defaults below load under the
	 * local service alone; star_codes_tests() pins the network default */
	const char config[] =
		"config base 'base'\n"
		"\toption channels '1'\n"
		"\toption star_codes_local '1'\n"
		"config channel 'channel1'\n"
		"\toption id '0'\n";
	struct cfg_fixture fixture;
	const struct cfg_channel *ch;

	CASE("empty channel loads");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(config));
	ch = &fixture.cfg->channel[0];
	CHECK(ch->present);

	CASE("empty channel session defaults");
	CHECK(ch->dtmf_mode == CFG_DTMF_INBAND);
	CHECK(ch->session_refresher == CFG_REFRESH_UAC);
	CHECK_UINT(ch->session_max_expires, 0);
	CHECK_UINT(ch->session_min_interval, 0);
	CHECK_UINT(ch->timer_c_ms, 0);

	CASE("empty channel decoded map default");
	CHECK_CSTR(ch->digit_map, "*xx.T|x.T");
	CHECK_UINT(ch->digit_esc, 1);

	CASE("empty channel digit timer defaults");
	CHECK_UINT(ch->start_digit_s, 16);
	CHECK_UINT(ch->interdigit_s, 4);
	CHECK_UINT(ch->interdigit_long_s, 16);

	CASE("empty channel call timer defaults");
	CHECK_UINT(ch->roh_s, 60);
	CHECK_UINT(ch->busy_tone_s, 40);
	CHECK_UINT(ch->flash_svc_s, 5);

	CASE("empty channel message waiting defaults");
	CHECK(ch->amwi);
	CHECK(ch->vmwi);

	CASE("empty channel call defaults");
	CHECK(ch->call_waiting_enabled);
	CHECK(!ch->call_waiting_local_set);
	CHECK_CSTR(ch->cw_enable_code, "*43#");
	CHECK_CSTR(ch->cw_disable_code, "#43#");
	CHECK_CSTR(ch->cw_query_code, "*#43#");
	CHECK_UINT(ch->local_conf_enabled, 1);

	/* both halves of the forward disconnect pair ship off */
	CASE("empty channel forward disconnect defaults");
	CHECK_UINT(ch->forward_disconnect_delay_ms, 0);
	CHECK(ch->forward_disconnect_battery == CFG_FWD_DISC_BATT_NONE);

	CASE("empty channel flash relay, caller ID, forward and do not disturb "
	     "defaults");
	CHECK(ch->sip_flash_relay == CFG_FLASH_RELAY_INFO);
	CHECK(ch->cid_remote_enable);
	CHECK(ch->cid_outgoing_enable);
	CHECK(!ch->cid_delivery_local_set);
	CHECK_CSTR(ch->cid_single_call_enable_exten, "*31#");
	CHECK_CSTR(ch->cid_single_call_disable_exten, "#31#");
	CHECK(ch->call_fwd_local);
	CHECK_UINT(ch->call_fwd_local_mode, 0);
	CHECK_CSTR(ch->call_fwd_uncond_uri, "");
	CHECK_CSTR(ch->call_fwd_busy_uri, "");
	CHECK_CSTR(ch->call_fwd_no_answer_uri, "");
	CHECK_UINT(ch->call_fwd_no_answer_timeout, 15);
	CHECK(!ch->do_not_disturb_enabled);
	CHECK_CSTR(ch->do_not_disturb_on_exten, "*78");
	CHECK_CSTR(ch->do_not_disturb_off_exten, "#78");

	/* The two persistent options carry no seed, because the obvious seed is
	 * the "*31#"/"#31#" of the one-call pair, and two options with one
	 * string cancel each other in the feature table. A config that does not
	 * name them therefore loads them empty, and the persistent rows match
	 * nothing until an operator writes strings of their own. */
	CASE("the persistent caller id pair ships unseeded");
	CHECK_CSTR(ch->cid_enable_exten, "");
	CHECK_CSTR(ch->cid_disable_exten, "");

	CASE("empty channel forwarding extension defaults");
	CHECK_CSTR(ch->call_fwd_uncond_exten, "*21#");
	CHECK_CSTR(ch->call_fwd_uncond_cancel_exten, "#21#");
	CHECK_CSTR(ch->call_fwd_uncond_query_exten, "*#21#");
	CHECK_CSTR(ch->call_fwd_busy_exten, "*67#");
	CHECK_CSTR(ch->call_fwd_busy_cancel_exten, "#67#");
	CHECK_CSTR(ch->call_fwd_busy_query_exten, "*#67#");
	CHECK_CSTR(ch->call_fwd_no_answer_exten, "*61#");
	CHECK_CSTR(ch->call_fwd_no_answer_cancel_exten, "#61#");
	CHECK_CSTR(ch->call_fwd_no_answer_query_exten, "*#61#");
	CHECK_CSTR(ch->call_fwd_unreachable_exten, "*62#");
	CHECK_CSTR(ch->call_fwd_unreachable_cancel_exten, "#62#");
	CHECK_CSTR(ch->call_fwd_all_exten, "*004#");
	/*
	 * The cancel code takes the '#'-leading form of its three siblings,
	 * because two options that carry one string cancel each other in the
	 * feature table and neither would ever run.
	 */
	CHECK_CSTR(ch->call_fwd_all_cancel_exten, "#004#");
	CHECK(strcmp(ch->call_fwd_all_exten, ch->call_fwd_all_cancel_exten) != 0);

	CASE("empty distinctive ring arrays stay dense");
	CHECK(!ch->distinctive_ring_enable);
	CHECK_CSTR(ch->distinctive_ring[0], "");
	CHECK_CSTR(ch->distinctive_ring[9], "");
	CHECK(!ch->distinctive_ring_def_set[0]);
	CHECK(!ch->distinctive_ring_def_set[9]);

	CASE("empty channel media defaults");
	/* the far-end upspeed recognizer ships on, so a silent config follows */
	CHECK(ch->fax_upspeed_follow);
	CHECK_UINT(ch->ptime_ms, 20);
	CHECK_UINT(ch->g711a_ptype, 8);
	CHECK_UINT(ch->g729a_ptype, 18);
	CHECK_UINT(ch->g726_ptype, 111);
	CHECK_UINT(ch->g722_ptype, 9);
	CHECK_UINT(ch->opus_ptype, 96);
	CHECK_UINT(ch->jitter_max, 250);
	CHECK_UINT(ch->rtp_tos, 184);
	CHECK_UINT(ch->rtcp_tos, 184);

	CASE("empty base defaults");
	CHECK(fixture.cfg->rtcp_enabled);
	CHECK_UINT(fixture.cfg->rtcp_interval, 0);
	CHECK_CSTR(fixture.cfg->alert_info_prefix, "Bellcore-dr");
	fixture_free(&fixture);
}

static void base_tests(void)
{
	const char no_base[] =
		"config channel 'channel1'\n"
		"\toption id '0'\n";
	struct cfg_fixture fixture;

	CASE("missing base section keeps every base default");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(no_base));
	CHECK(!fixture.cfg->enabled);
	CHECK_UINT(fixture.cfg->channels, 0);
	CHECK_CSTR(fixture.cfg->local_interface, "wan");
	CHECK_CSTR(fixture.cfg->transport, "udp");
	CHECK_CSTR(fixture.cfg->region, "US");
	CHECK_UINT(fixture.cfg->restrict_source, 2);
	CHECK_UINT(fixture.cfg->options_interval, 0);
	CHECK(fixture.cfg->keepalive_enabled);
	CHECK_UINT(fixture.cfg->keepalive_interval, 30);
	CHECK(fixture.cfg->dns_cache_update_mode);
	CHECK(fixture.cfg->dns_move_unregister);
	CHECK(fixture.cfg->dns_srv_mode == CFG_DNS_SRV_AUTO);
	CHECK_UINT(fixture.cfg->dns_linger, 3600);
	CHECK(!fixture.cfg->mwi_subscribe);
	CHECK(!fixture.cfg->star_codes_local);
	CHECK(fixture.cfg->rtcp_enabled);
	CHECK_UINT(fixture.cfg->rtcp_interval, 0);
	CHECK_CSTR(fixture.cfg->alert_info_prefix, "Bellcore-dr");
	CHECK_CSTR(fixture.cfg->tls_cert_path, "");
	CHECK_CSTR(fixture.cfg->tls_cert_username, "");
	CHECK_CSTR(fixture.cfg->tls_cert_password, "");
	CHECK(!fixture.cfg->tls_server_verification_enabled);
	CHECK(!fixture.cfg->tls_keylog_enabled);
	fixture_free(&fixture);

	CASE("invalid restrict source uses enum default");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n\toption restrict_source '5'\n"));
	CHECK_UINT(fixture.cfg->restrict_source, 2);
	fixture_free(&fixture);

	CASE("zero RTCP interval clamps to one");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n\toption rtcp_interval '0'\n"));
	CHECK_UINT(fixture.cfg->rtcp_interval, 1);
	fixture_free(&fixture);
}

/*
 * The region row is free-form, and nothing in the daemon reads its type.
 * region_iso3_get() in line/line.c maps 32 alpha-2 codes, and passes every name
 * of three or more characters to the backend in upper case. A closed enum row
 * would state a rule that the loader does not run, and the descriptor is the
 * only place that holds the answer.
 *
 * It is also the one regional input of the schema, because every regional
 * value comes from the region data of the backend. The schema has no base row
 * for a regional format and no region section type. A config that carries
 * them loads, and their values reach no field.
 */
static void region_schema_test(void)
{
	static const char *const gone[] = {
		"cid_format", "mwi_format", "vapp_country_code",
	};
	const struct cfg_option_desc *d;
	struct cfg_fixture fixture;
	unsigned int found = 0;
	unsigned int i;

	CASE("schema: the region row is declared free-form");
	for (i = 0; cfg_base_options[i].name; i++) {
		d = &cfg_base_options[i];
		if (strcmp(d->name, "region"))
			continue;

		found++;
		CHECK_UINT(d->type, CFG_STRING);
		CHECK(d->enum_vals == NULL);
		CHECK_CSTR(d->def, "US");
		CHECK_UINT(d->support, SUP_SUPPORTED);
		CHECK_UINT(d->apply_class, APPLY_RESTART_ALL);
	}
	CHECK_UINT(found, 1);

	CASE("schema: the base format overrides are not rows");
	for (i = 0; i < ARRAY_SIZE(gone); i++) {
		found = 0;
		for (d = cfg_base_options; d->name; d++) {
			if (!strcmp(d->name, gone[i]))
				found++;
		}
		CHECK_UINT(found, 0);
	}

	CASE("a region section and a base format override load silently");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n\toption region 'GB'\n"
		"\toption cid_format 'dtmf-dtas'\n"
		"config region 'GB'\n\toption cid_format 'etsi-dtas'\n"
		"\toption hook_flash_min '35'\n"
		"\toption distinctRingPattern0 '2,400,200,400,2000,0,0'\n"
		"config channel 'channel1'\n\toption id '0'\n"));
	CHECK_UINT(warn_capture_count("cid_format"), 0);
	CHECK_UINT(warn_capture_count("region"), 0);
	CHECK_CSTR(fixture.cfg->region, "GB");
	fixture_free(&fixture);
}

/*
 * Registration retry, the DNS TTL mode and the UAC local port. Each option
 * gets one check of the configured value and one of the schema default. The
 * group is about the options existing at all.
 */
static void control_option_tests(void)
{
	struct cfg_fixture fixture;

	CASE("sip_register_retry loads the configured value");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n\toption channels '1'\n"
		"\toption dns_cache_update_mode '0'\n"
		"config channel 'channel1'\n\toption id '0'\n"
		"\toption sip_register_retry '17'\n"
		"\toption sip_local_port '5080'\n"));
	CHECK_UINT(fixture.cfg->channel[0].sip_register_retry, 17);

	CASE("dns_cache_update_mode loads the configured value");
	CHECK(!fixture.cfg->dns_cache_update_mode);

	CASE("sip_local_port loads the configured value");
	CHECK_UINT(fixture.cfg->channel[0].sip_local_port, 5080);
	fixture_free(&fixture);

	CASE("dns_move_unregister loads the configured value");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n\toption dns_move_unregister '0'\n"));
	CHECK(!fixture.cfg->dns_move_unregister);
	fixture_free(&fixture);

	CASE("dns_move_unregister carries its schema default when the "
	     "config is silent");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load("config base 'base'\n"));
	CHECK(fixture.cfg->dns_move_unregister);
	fixture_free(&fixture);

	CASE("dns_srv_mode carries its schema default when the config is silent");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load("config base 'base'\n"));
	CHECK(fixture.cfg->dns_srv_mode == CFG_DNS_SRV_AUTO);
	fixture_free(&fixture);

	CASE("dns_srv_mode loads always");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n\toption dns_srv_mode 'always'\n"));
	CHECK(fixture.cfg->dns_srv_mode == CFG_DNS_SRV_ALWAYS);
	fixture_free(&fixture);

	CASE("dns_srv_mode loads never");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n\toption dns_srv_mode 'never'\n"));
	CHECK(fixture.cfg->dns_srv_mode == CFG_DNS_SRV_NEVER);
	fixture_free(&fixture);

	CASE("dns_srv_mode loads auto");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n\toption dns_srv_mode 'auto'\n"));
	CHECK(fixture.cfg->dns_srv_mode == CFG_DNS_SRV_AUTO);
	fixture_free(&fixture);

	CASE("an unknown dns_srv_mode word warns and takes the default");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n\toption dns_srv_mode 'sometimes'\n"));
	CHECK_UINT(warn_capture_count(
		"voip base dns_srv_mode value 'sometimes' is invalid, using "
		"default 'auto'"), 1);
	warn_capture_end();
	CHECK(fixture.cfg->dns_srv_mode == CFG_DNS_SRV_AUTO);
	fixture_free(&fixture);

	CASE("dns_linger loads the configured value");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n\toption dns_linger '900'\n"));
	CHECK_UINT(fixture.cfg->dns_linger, 900);
	fixture_free(&fixture);

	CASE("dns_linger zero turns the retention off");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n\toption dns_linger '0'\n"));
	CHECK_UINT(fixture.cfg->dns_linger, 0);
	fixture_free(&fixture);

	CASE("a broken dns_linger falls back to the schema default");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n\toption dns_linger 'broken'\n"));
	CHECK_UINT(fixture.cfg->dns_linger, 3600);
	fixture_free(&fixture);

	CASE("the three carry their schema defaults when the config is silent");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n\toption channels '1'\n"
		"config channel 'channel1'\n\toption id '0'\n"));
	CHECK_UINT(fixture.cfg->channel[0].sip_register_retry, 5);
	CHECK(fixture.cfg->dns_cache_update_mode);
	CHECK_UINT(fixture.cfg->channel[0].sip_local_port, 5060);
	fixture_free(&fixture);

	CASE("a sip_local_port above the port range clamps instead of wrapping");
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(
		"\toption sip_local_port '65536'\n"));
	CHECK_UINT(fixture.cfg->channel[0].sip_local_port, 65535);
	fixture_free(&fixture);
}

static void m2_invalid_tests(void)
{
	struct cfg_fixture fixture;
	const struct cfg_channel *ch;

	CASE("invalid call enums use their schema defaults");
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(
		"\toption sip_flash_relay 'broken'\n"
		"\toption call_fwd_local_mode '3'\n"));
	ch = &fixture.cfg->channel[0];
	CHECK(ch->sip_flash_relay == CFG_FLASH_RELAY_INFO);
	CHECK_UINT(ch->call_fwd_local_mode, 0);
	fixture_free(&fixture);

	CASE("RFC2833 flash relay value loads");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption sip_flash_relay 'rfc2833'\n"));
	CHECK(fixture.cfg->channel[0].sip_flash_relay ==
	      CFG_FLASH_RELAY_RFC2833);
	fixture_free(&fixture);

	CASE("an absent flash relay loads the schema default info");
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(""));
	CHECK(fixture.cfg->channel[0].sip_flash_relay == CFG_FLASH_RELAY_INFO);
	fixture_free(&fixture);

	CASE("invalid call boolean takes its schema default");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption cid_remote_enable 'broken'\n"));
	CHECK(fixture.cfg->channel[0].cid_remote_enable);
	fixture_free(&fixture);

	CASE("invalid forwarding timeout uses the schema default");
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(
		"\toption call_fwd_no_answer_timeout 'broken'\n"));
	CHECK_UINT(fixture.cfg->channel[0].call_fwd_no_answer_timeout, 15);
	fixture_free(&fixture);

	CASE("distinctive ring definition above range clamps");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption distinctive_ring_def_9 '8'\n"));
	CHECK(fixture.cfg->channel[0].distinctive_ring_def_set[9]);
	CHECK_UINT(fixture.cfg->channel[0].distinctive_ring_def[9], 7);
	fixture_free(&fixture);

	CASE("non-numeric distinctive ring definition stays unset");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption distinctive_ring_def_9 'broken'\n"));
	CHECK(!fixture.cfg->channel[0].distinctive_ring_def_set[9]);
	fixture_free(&fixture);

	CASE("invalid optional CID delivery value stays unset");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption cid_delivery_local 'broken'\n"));
	CHECK(!fixture.cfg->channel[0].cid_delivery_local_set);
	CHECK(!fixture.cfg->channel[0].cid_delivery_local);
	fixture_free(&fixture);

}

static struct cfg_fixture channel_option_load(const char *option)
{
	char config[2048];

	snprintf(config, sizeof(config),
		 "config channel 'channel1'\n\toption id '0'\n%s", option);

	return fixture_load(config);
}

static struct cfg_fixture hal_option_load(const char *option)
{
	char config[2048];

	snprintf(config, sizeof(config), "config hal 'hal'\n%s", option);

	return fixture_load(config);
}

/*
 * The forward disconnect vocabulary. The keyword map is the whole loader
 * contract for the pair; which combinations enable anything is line/line.c work
 * and test_line owns it.
 */
static void fwd_disc_tests(void)
{
	struct cfg_fixture fixture;

	CASE("forward disconnect battery reverse loads");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption forward_disconnect_battery 'reverse'\n"));
	CHECK(fixture.cfg->channel[0].forward_disconnect_battery ==
	      CFG_FWD_DISC_BATT_REVERSE);
	fixture_free(&fixture);

	CASE("forward disconnect battery remove loads as its own value");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption forward_disconnect_battery 'remove'\n"));
	CHECK(fixture.cfg->channel[0].forward_disconnect_battery ==
	      CFG_FWD_DISC_BATT_REMOVE);
	fixture_free(&fixture);

	CASE("forward disconnect battery none loads");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption forward_disconnect_battery 'none'\n"));
	CHECK(fixture.cfg->channel[0].forward_disconnect_battery ==
	      CFG_FWD_DISC_BATT_NONE);
	fixture_free(&fixture);

	CASE("an unknown forward disconnect battery takes the schema default");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption forward_disconnect_battery 'ground'\n"));
	CHECK(fixture.cfg->channel[0].forward_disconnect_battery ==
	      CFG_FWD_DISC_BATT_NONE);
	fixture_free(&fixture);

	/* 250, 500, 750, 900, 1000 and 2000 ms are the values the schema
	 * documents, and the uint policy takes any value */
	CASE("a forward disconnect delay outside the six documented values loads");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption forward_disconnect_delay '1234'\n"));
	CHECK_UINT(fixture.cfg->channel[0].forward_disconnect_delay_ms, 1234);
	fixture_free(&fixture);

	CASE("the lowest and the highest documented forward disconnect delay "
	     "values load");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption forward_disconnect_delay '250'\n"));
	CHECK_UINT(fixture.cfg->channel[0].forward_disconnect_delay_ms, 250);
	fixture_free(&fixture);
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption forward_disconnect_delay '2000'\n"));
	CHECK_UINT(fixture.cfg->channel[0].forward_disconnect_delay_ms, 2000);
	fixture_free(&fixture);
}

static void local_conf_tests(void)
{
	struct cfg_fixture fixture;

	CASE("local conference transfer value loads");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption local_conf_enabled '2'\n"));
	CHECK_UINT(fixture.cfg->channel[0].local_conf_enabled, 2);
	fixture_free(&fixture);

	CASE("local conference explicit one loads");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption local_conf_enabled '1'\n"));
	CHECK_UINT(fixture.cfg->channel[0].local_conf_enabled, 1);
	fixture_free(&fixture);

	CASE("local conference word on maps to one");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption local_conf_enabled 'on'\n"));
	CHECK_UINT(fixture.cfg->channel[0].local_conf_enabled, 1);
	fixture_free(&fixture);

	CASE("local conference word n maps to zero");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption local_conf_enabled 'n'\n"));
	CHECK_UINT(fixture.cfg->channel[0].local_conf_enabled, 0);
	fixture_free(&fixture);

	CASE("local conference word no maps to zero");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption local_conf_enabled 'no'\n"));
	CHECK_UINT(fixture.cfg->channel[0].local_conf_enabled, 0);
	fixture_free(&fixture);

	CASE("local conference word false maps to zero");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption local_conf_enabled 'false'\n"));
	CHECK_UINT(fixture.cfg->channel[0].local_conf_enabled, 0);
	fixture_free(&fixture);

	CASE("local conference word off maps to zero");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption local_conf_enabled 'off'\n"));
	CHECK_UINT(fixture.cfg->channel[0].local_conf_enabled, 0);
	fixture_free(&fixture);

	CASE("local conference absent uses default");
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(""));
	CHECK_UINT(fixture.cfg->channel[0].local_conf_enabled, 1);
	fixture_free(&fixture);

	CASE("invalid local conference uses default");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption local_conf_enabled '5'\n"));
	CHECK_UINT(fixture.cfg->channel[0].local_conf_enabled, 1);
	fixture_free(&fixture);
}

/*
 * The echo canceller vocabulary and the half decibel loss plan. The loader
 * contract is the values and their presence. line/line.c decides which
 * combination reaches the canceller and which gain takes a loss, and test_line
 * covers that.
 */
static void lec_gain_tests(void)
{
	struct cfg_fixture fixture;
	const struct cfg_channel *ch;

	CASE("a silent config leaves the canceller to the daemon policy");
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(""));
	ch = &fixture.cfg->channel[0];
	CHECK(!ch->lec_control_set);
	CHECK(!ch->lec_control);
	CHECK(!ch->lec_nlp_set);
	/* the schema default of lec_enable is 1, so it always loads a value and
	 * needs no presence flag of its own */
	CHECK(ch->lec_enable);
	/* both tone options default to 0, the G.168 behaviour, so a silent
	 * config asks for the recommendation and needs no presence flag either */
	CHECK(!ch->lec_tone_latch);
	CHECK(!ch->lec_tone_force_disable);
	CHECK(!ch->half_db_rx_loss);
	CHECK(!ch->half_db_tx_loss);
	fixture_free(&fixture);

	CASE("manual control with the canceller off loads");
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(
		"\toption lec_control '1'\n"
		"\toption lec_enable '0'\n"));
	ch = &fixture.cfg->channel[0];
	CHECK(ch->lec_control_set);
	CHECK(ch->lec_control);
	CHECK(!ch->lec_enable);
	fixture_free(&fixture);

	CASE("an explicit lec_control 0 is PRESENT and false");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption lec_control '0'\n"));
	CHECK(fixture.cfg->channel[0].lec_control_set);
	CHECK(!fixture.cfg->channel[0].lec_control);
	fixture_free(&fixture);

	CASE("an unreadable lec_control stays unset");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption lec_control 'broken'\n"));
	CHECK(!fixture.cfg->channel[0].lec_control_set);
	CHECK(!fixture.cfg->channel[0].lec_control);
	fixture_free(&fixture);

	CASE("an explicit lec_nlp 0 is PRESENT and false");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption lec_nlp '0'\n"));
	CHECK(fixture.cfg->channel[0].lec_nlp_set);
	CHECK(!fixture.cfg->channel[0].lec_nlp);
	fixture_free(&fixture);

	CASE("an explicit lec_nlp 1 is PRESENT and true");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption lec_nlp '1'\n"));
	CHECK(fixture.cfg->channel[0].lec_nlp_set);
	CHECK(fixture.cfg->channel[0].lec_nlp);
	fixture_free(&fixture);

	CASE("an unreadable lec_nlp stays unset");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption lec_nlp 'broken'\n"));
	CHECK(!fixture.cfg->channel[0].lec_nlp_set);
	CHECK(!fixture.cfg->channel[0].lec_nlp);
	fixture_free(&fixture);

	CASE("an explicit lec_enable 1 loads without a control option");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption lec_enable '1'\n"));
	CHECK(fixture.cfg->channel[0].lec_enable);
	CHECK(!fixture.cfg->channel[0].lec_control_set);
	fixture_free(&fixture);

	/* the two tone options carry no presence flag, so an explicit 1 is the
	 * whole contract; lec_control has no say over either of them */
	CASE("both tone options load as their own values, with no lec_control");
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(
		"\toption lec_tone_latch '1'\n"
		"\toption lec_tone_force_disable '1'\n"));
	ch = &fixture.cfg->channel[0];
	CHECK(ch->lec_tone_latch);
	CHECK(ch->lec_tone_force_disable);
	CHECK(!ch->lec_control_set);
	fixture_free(&fixture);

	CASE("one tone option leaves the other at its default");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption lec_tone_force_disable '1'\n"));
	CHECK(!fixture.cfg->channel[0].lec_tone_latch);
	CHECK(fixture.cfg->channel[0].lec_tone_force_disable);
	fixture_free(&fixture);

	CASE("an unreadable tone option takes its schema default of 0");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption lec_tone_latch 'broken'\n"));
	CHECK(!fixture.cfg->channel[0].lec_tone_latch);
	fixture_free(&fixture);

	CASE("both half decibel losses load as their own values");
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(
		"\toption half_db_rx_loss '1'\n"
		"\toption half_db_tx_loss '1'\n"));
	CHECK(fixture.cfg->channel[0].half_db_rx_loss);
	CHECK(fixture.cfg->channel[0].half_db_tx_loss);
	fixture_free(&fixture);

	CASE("one half decibel loss leaves the other alone");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption half_db_rx_loss '1'\n"));
	CHECK(fixture.cfg->channel[0].half_db_rx_loss);
	CHECK(!fixture.cfg->channel[0].half_db_tx_loss);
	fixture_free(&fixture);

	CASE("an unreadable half decibel loss takes its schema default of 0");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption half_db_tx_loss 'broken'\n"));
	CHECK(!fixture.cfg->channel[0].half_db_tx_loss);
	fixture_free(&fixture);
}

/*
 * The SLIC analog pair, whole dB in the range 0 to -20 dB.
 * The loader contract is the value, the bounds and the clamp; the doubling
 * into the half-dB HAL pair is line/line.c work, and test_line owns it.
 */
static void ps_gain_tests(void)
{
	enum {
		DUMP_CHANNEL,
		DUMP_MAX,
	};
	enum {
		CHANNEL_PS_TX,
		CHANNEL_PS_RX,
		CHANNEL_MAX,
	};
	static const struct blobmsg_policy dump_policy[DUMP_MAX] = {
		[DUMP_CHANNEL] = { .name = "channel", .type = BLOBMSG_TYPE_ARRAY },
	};
	static const struct blobmsg_policy channel_policy[CHANNEL_MAX] = {
		[CHANNEL_PS_TX] = {
			.name = "ps_tx_gain",
			.type = BLOBMSG_TYPE_INT32,
		},
		[CHANNEL_PS_RX] = {
			.name = "ps_rx_gain",
			.type = BLOBMSG_TYPE_INT32,
		},
	};
	struct blob_attr *dump_fields[DUMP_MAX] = { 0 };
	struct blob_attr *channel_fields[CHANNEL_MAX] = { 0 };
	struct blob_attr *channel_entry;
	struct cfg_fixture fixture;
	struct blob_buf buf = { 0 };

	CASE("the analog pair defaults load as -3 and -9");
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(""));
	CHECK(fixture.cfg->channel[0].ps_tx_gain == -3);
	CHECK(fixture.cfg->channel[0].ps_rx_gain == -9);
	fixture_free(&fixture);

	CASE("explicit analog values load");
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(
		"\toption ps_tx_gain '-12'\n"
		"\toption ps_rx_gain '-6'\n"));
	CHECK(fixture.cfg->channel[0].ps_tx_gain == -12);
	CHECK(fixture.cfg->channel[0].ps_rx_gain == -6);
	fixture_free(&fixture);

	/* the uvoice-wide convention clamps an out-of-range value and warns */
	CASE("a value below the schema floor clamps to -20 with a warning");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption ps_tx_gain '-21'\n"));
	CHECK(fixture.cfg->channel[0].ps_tx_gain == -20);
	CHECK_UINT(warn_capture_count("voip ps_tx_gain -21 below -20, clamped"), 1);
	fixture_free(&fixture);

	CASE("a positive value clamps to 0 with a warning");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption ps_rx_gain '1'\n"));
	CHECK(fixture.cfg->channel[0].ps_rx_gain == 0);
	CHECK_UINT(warn_capture_count("voip ps_rx_gain 1 above 0, clamped"), 1);
	fixture_free(&fixture);
	warn_capture_end();

	CASE("the dump carries both analog rows");
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(""));
	blobmsg_buf_init(&buf);
	CHECK(buf.head != NULL);
	if (!buf.head) {
		blob_buf_free(&buf);
		fixture_free(&fixture);
		return;
	}

	cfg_dump(fixture.cfg, &buf);
	blobmsg_parse_attr(dump_policy, DUMP_MAX, dump_fields, buf.head);
	channel_entry = dump_fields[DUMP_CHANNEL] ?
		(struct blob_attr *)blobmsg_data(dump_fields[DUMP_CHANNEL]) : NULL;
	if (channel_entry)
		blobmsg_parse_attr(channel_policy, CHANNEL_MAX, channel_fields,
				   channel_entry);
	CHECK(channel_fields[CHANNEL_PS_TX] != NULL);
	CHECK(channel_fields[CHANNEL_PS_RX] != NULL);
	if (channel_fields[CHANNEL_PS_TX])
		CHECK((int)blobmsg_get_u32(channel_fields[CHANNEL_PS_TX]) == -3);
	if (channel_fields[CHANNEL_PS_RX])
		CHECK((int)blobmsg_get_u32(channel_fields[CHANNEL_PS_RX]) == -9);

	blob_buf_free(&buf);
	fixture_free(&fixture);
}

enum {
	PRACK_DUMP_CHANNEL,
	PRACK_DUMP_MAX,
};

enum {
	PRACK_CH_PRACK,
	PRACK_CH_EARLY_MEDIA,
	PRACK_CH_MAX,
};

static const struct blobmsg_policy prack_dump_policy[PRACK_DUMP_MAX] = {
	[PRACK_DUMP_CHANNEL] = {
		.name = "channel",
		.type = BLOBMSG_TYPE_ARRAY,
	},
};

static const struct blobmsg_policy prack_channel_policy[PRACK_CH_MAX] = {
	[PRACK_CH_PRACK] = {
		.name = "prack",
		.type = BLOBMSG_TYPE_BOOL,
	},
	[PRACK_CH_EARLY_MEDIA] = {
		.name = "early_media_enabled",
		.type = BLOBMSG_TYPE_BOOL,
	},
};

static bool prack_dump_fields_get(const struct cfg *cfg, struct blob_buf *buf,
				  struct blob_attr **channel_fields)
{
	struct blob_attr *dump_fields[PRACK_DUMP_MAX] = { 0 };
	struct blob_attr *channel_entry;

	blobmsg_buf_init(buf);
	CHECK(buf->head != NULL);
	if (!buf->head)
		return false;

	cfg_dump(cfg, buf);
	blobmsg_parse_attr(prack_dump_policy, PRACK_DUMP_MAX, dump_fields,
			   buf->head);
	channel_entry = dump_fields[PRACK_DUMP_CHANNEL];
	if (channel_entry)
		blobmsg_parse_attr(prack_channel_policy, PRACK_CH_MAX,
				   channel_fields, blobmsg_data(channel_entry));

	return true;
}

/*
 * The reliable provisional switch and the early media option beside it. The
 * two are independent: early_media_enabled defaults to 1 at either prack
 * setting, and an explicit value wins at either prack setting.
 */
static void prack_tests(void)
{
	struct blob_attr *channel_fields[PRACK_CH_MAX] = { 0 };
	const struct cfg_option_desc *d;
	struct cfg_fixture fixture;
	struct blob_buf buf = { 0 };
	unsigned int found = 0;
	unsigned int i;

	CASE("an explicit prack 1 loads as true");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption prack '1'\n"));
	CHECK(fixture.cfg->channel[0].prack);
	fixture_free(&fixture);

	CASE("a config that names no prack loads it as false");
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(""));
	CHECK(!fixture.cfg->channel[0].prack);
	fixture_free(&fixture);

	CASE("a malformed prack warns once and takes its schema default");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption prack 'maybe'\n"));
	CHECK(!fixture.cfg->channel[0].prack);
	CHECK_UINT(warn_capture_count(
		"voip prack value 'maybe' is not a boolean"), 1);
	fixture_free(&fixture);
	warn_capture_end();

	CASE("a config that names no early media reads it as true");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption prack '0'\n"));
	CHECK(fixture.cfg->channel[0].early_media_enabled);
	fixture_free(&fixture);
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption prack '1'\n"));
	CHECK(fixture.cfg->channel[0].early_media_enabled);
	fixture_free(&fixture);

	CASE("an explicit early media 0 beside prack 1 stays false");
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(
		"\toption prack '1'\n"
		"\toption early_media_enabled '0'\n"));
	CHECK(fixture.cfg->channel[0].prack);
	CHECK(!fixture.cfg->channel[0].early_media_enabled);
	fixture_free(&fixture);

	CASE("an explicit early media 1 beside prack 0 stays true");
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(
		"\toption prack '0'\n"
		"\toption early_media_enabled '1'\n"));
	CHECK(!fixture.cfg->channel[0].prack);
	CHECK(fixture.cfg->channel[0].early_media_enabled);
	fixture_free(&fixture);

	CASE("a malformed early media warns once and takes its schema "
	     "default 1");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(
		"\toption prack '0'\n"
		"\toption early_media_enabled 'x'\n"));
	CHECK(fixture.cfg->channel[0].early_media_enabled);
	CHECK_UINT(warn_capture_count(
		"voip early_media_enabled value 'x' is not a boolean"), 1);
	fixture_free(&fixture);
	warn_capture_end();

	CASE("schema: the early media row declares default 1 and the parser "
	     "agrees");
	for (i = 0; cfg_channel_options[i].name; i++) {
		d = &cfg_channel_options[i];
		if (strcmp(d->name, "early_media_enabled"))
			continue;

		found++;
		CHECK_UINT(d->type, CFG_BOOL);
		CHECK(d->def != NULL);
		if (d->def)
			CHECK_CSTR(d->def, "1");
	}
	CHECK_UINT(found, 1);
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(""));
	CHECK(fixture.cfg->channel[0].early_media_enabled);
	fixture_free(&fixture);

	CASE("the channel dump carries prack with the value the config named");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption prack '1'\n"));
	if (!prack_dump_fields_get(fixture.cfg, &buf, channel_fields)) {
		blob_buf_free(&buf);
		fixture_free(&fixture);
		return;
	}
	CHECK(channel_fields[PRACK_CH_PRACK] != NULL);
	if (channel_fields[PRACK_CH_PRACK])
		CHECK(blobmsg_get_bool(channel_fields[PRACK_CH_PRACK]));
	/* the dump reports the defaulted early media, true */
	CHECK(channel_fields[PRACK_CH_EARLY_MEDIA] != NULL);
	if (channel_fields[PRACK_CH_EARLY_MEDIA])
		CHECK(blobmsg_get_bool(channel_fields[PRACK_CH_EARLY_MEDIA]));
	blob_buf_free(&buf);
	fixture_free(&fixture);

	CASE("the channel dump reports a defaulted prack as false");
	memset(channel_fields, 0, sizeof(channel_fields));
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(""));
	if (!prack_dump_fields_get(fixture.cfg, &buf, channel_fields)) {
		blob_buf_free(&buf);
		fixture_free(&fixture);
		return;
	}
	CHECK(channel_fields[PRACK_CH_PRACK] != NULL);
	if (channel_fields[PRACK_CH_PRACK])
		CHECK(!blobmsg_get_bool(channel_fields[PRACK_CH_PRACK]));
	blob_buf_free(&buf);
	fixture_free(&fixture);
}

/*
 * A present bool value that the parser refuses reads as an absent option. So
 * the schema default applies, no presence flag rises, and the read warns
 * once. "false" and "the schema default" differ only on the options whose
 * default is 1, so the cases check the classes one by one.
 */
static void bool_malformed_tests(void)
{
	struct cfg_channel absent = { 0 };
	struct cfg_fixture fixture;
	const struct cfg_channel *ch;

	CASE("a malformed lec_enable reads TRUE, its schema default");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption lec_enable 'broken'\n"));
	CHECK(fixture.cfg->channel[0].lec_enable);
	fixture_free(&fixture);

	CASE("every malformed bool reads exactly as the absent option does");
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(""));
	absent = fixture.cfg->channel[0];
	fixture_free(&fixture);
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(
		"\toption lec_control 'broken'\n"
		"\toption lec_enable 'broken'\n"
		"\toption lec_nlp 'broken'\n"
		"\toption call_waiting_local 'broken'\n"
		"\toption cid_delivery_local 'broken'\n"
		"\toption do_not_disturb_enabled 'broken'\n"
		"\toption lec_tone_latch 'broken'\n"
		"\toption lec_tone_force_disable 'broken'\n"
		"\toption half_db_tx_loss 'broken'\n"));
	ch = &fixture.cfg->channel[0];
	CHECK(ch->lec_control == absent.lec_control);
	CHECK(ch->lec_enable == absent.lec_enable);
	CHECK(ch->lec_nlp == absent.lec_nlp);
	CHECK(ch->lec_tone_latch == absent.lec_tone_latch);
	CHECK(ch->lec_tone_force_disable == absent.lec_tone_force_disable);
	CHECK(ch->call_waiting_local == absent.call_waiting_local);
	CHECK(ch->cid_delivery_local == absent.cid_delivery_local);
	CHECK(ch->do_not_disturb_enabled == absent.do_not_disturb_enabled);
	CHECK(ch->half_db_tx_loss == absent.half_db_tx_loss);

	CASE("and it raises none of the presence flags");
	CHECK(!ch->lec_control_set);
	CHECK(!ch->lec_nlp_set);
	CHECK(!ch->call_waiting_local_set);
	CHECK(!ch->cid_delivery_local_set);
	fixture_free(&fixture);

	CASE("a malformed call_waiting_local stays unset and reads as absent");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption call_waiting_local 'broken'\n"));
	CHECK(!fixture.cfg->channel[0].call_waiting_local_set);
	CHECK(!fixture.cfg->channel[0].call_waiting_local);
	fixture_free(&fixture);

	CASE("a malformed bool whose schema default is 0 still reads false");
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(
		"\toption do_not_disturb_enabled 'broken'\n"
		"\toption half_db_rx_loss 'broken'\n"));
	CHECK(!fixture.cfg->channel[0].do_not_disturb_enabled);
	CHECK(!fixture.cfg->channel[0].half_db_rx_loss);
	fixture_free(&fixture);

	CASE("and one with no schema default at all reads false");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption lec_control 'broken'\n"));
	CHECK(!fixture.cfg->channel[0].lec_control);
	CHECK(!fixture.cfg->channel[0].lec_control_set);
	fixture_free(&fixture);

	CASE("the rule holds for the base section");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n\toption channels '1'\n"
		"\toption enabled 'broken'\n"
		"\toption keepalive_enabled 'broken'\n"
		"\toption dns_cache_update_mode 'broken'\n"
		"\toption dns_move_unregister 'broken'\n"
		"\toption mwi_subscribe 'broken'\n"));
	CHECK(!fixture.cfg->enabled);
	CHECK(fixture.cfg->keepalive_enabled);
	CHECK(fixture.cfg->dns_cache_update_mode);
	CHECK(fixture.cfg->dns_move_unregister);
	CHECK(!fixture.cfg->mwi_subscribe);
	fixture_free(&fixture);

	CASE("and for the codec section");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config codec 'broken_codec'\n"
		"\toption enabled 'broken'\n"
		"\toption silence_suppression 'broken'\n"));
	CHECK_UINT(fixture.cfg->codecs, 1);
	CHECK(fixture.cfg->codec[0].enabled);
	CHECK(!fixture.cfg->codec[0].silence_suppression);
	fixture_free(&fixture);

	/* libuci discards an option whose value is empty, so the loader sees no
	 * option at all here; the case pins the observable behaviour, not the
	 * empty-string guard inside opt_bool */
	CASE("an empty bool value keeps the absent path it already had");
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(
		"\toption lec_enable ''\n"
		"\toption lec_nlp ''\n"
		"\toption call_waiting_local ''\n"));
	ch = &fixture.cfg->channel[0];
	CHECK(ch->lec_enable);
	CHECK(!ch->lec_nlp_set);
	CHECK(!ch->call_waiting_local_set);
	fixture_free(&fixture);

	CASE("a malformed read warns once, naming the option and the text");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption lec_enable 'broken'\n"));
	CHECK(fixture.cfg->channel[0].lec_enable);
	CHECK_UINT(warn_capture_count("voip lec_enable value 'broken' is not a boolean"),
		   1);
	fixture_free(&fixture);

	/* a count that closed the capture answered 0 to every question after the
	 * first, which made an expected 0 unable to fail anywhere in this file */
	CASE("one capture answers the same warning twice");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption lec_enable 'broken'\n"));
	CHECK_UINT(warn_capture_count("voip lec_enable value 'broken' is not a boolean"),
		   1);
	CHECK_UINT(warn_capture_count("voip lec_enable value 'broken' is not a boolean"),
		   1);
	fixture_free(&fixture);

	/* the optional reader falls through to opt_bool, so the warn has two
	 * chances to land and must still land once */
	CASE("the optional bool path warns once, not twice");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption lec_nlp 'broken'\n"));
	CHECK(!fixture.cfg->channel[0].lec_nlp_set);
	CHECK_UINT(warn_capture_count("voip lec_nlp value"), 1);
	fixture_free(&fixture);

	CASE("an empty or valid bool value warns not at all");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(
		"\toption lec_enable ''\n"
		"\toption lec_nlp ''\n"
		"\toption half_db_rx_loss '1'\n"));
	CHECK(fixture.cfg->channel[0].half_db_rx_loss);
	CHECK_UINT(warn_capture_count("is not a boolean"), 0);
	fixture_free(&fixture);
	warn_capture_end();
}

/*
 * The valid spellings, on an option whose schema default is the opposite of
 * the expected result. A spelling that the parser dropped silently would read
 * as the default, and would fail here instead of passing by accident.
 *
 * get_bool() in the OpenWrt base-files functions.sh accepts "enabled" and
 * "disabled". A config that reads as off there must therefore not read here
 * as the schema default.
 */
static void bool_spelling_tests(void)
{
	static const char *const true_vals[] = {
		"1", "y", "Y", "yes", "YES", "true", "TRUE", "on", "ON",
		"enabled", "Enabled", "ENABLED",
	};
	static const char *const false_vals[] = {
		"0", "n", "N", "no", "NO", "false", "FALSE", "off", "OFF",
		"disabled", "Disabled", "DISABLED",
	};
	struct cfg_fixture fixture;
	char option[64];
	unsigned int i;

	CASE("every true spelling loads on an option whose default is 0");
	for (i = 0; i < ARRAY_SIZE(true_vals); i++) {
		snprintf(option, sizeof(option), "\toption half_db_rx_loss '%s'\n",
			 true_vals[i]);
		FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(option));
		CHECK(fixture.cfg->channel[0].half_db_rx_loss);
		fixture_free(&fixture);
	}

	CASE("every false spelling loads on an option whose default is 1");
	for (i = 0; i < ARRAY_SIZE(false_vals); i++) {
		snprintf(option, sizeof(option), "\toption lec_enable '%s'\n",
			 false_vals[i]);
		FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(option));
		CHECK(!fixture.cfg->channel[0].lec_enable);
		fixture_free(&fixture);
	}

	/* get_bool() reads this config as off, and the schema default of
	 * lec_enable is 1 */
	CASE("lec_enable 'disabled' turns the canceller off and warns nothing");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption lec_enable 'disabled'\n"));
	CHECK(!fixture.cfg->channel[0].lec_enable);
	CHECK_UINT(warn_capture_count("is not a boolean"), 0);
	fixture_free(&fixture);
	warn_capture_end();

	CASE("an enabled or disabled spelling on an optional bool raises the "
	     "presence flag");
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(
		"\toption lec_nlp 'disabled'\n"
		"\toption lec_control 'enabled'\n"));
	CHECK(fixture.cfg->channel[0].lec_nlp_set);
	CHECK(!fixture.cfg->channel[0].lec_nlp);
	CHECK(fixture.cfg->channel[0].lec_control_set);
	CHECK(fixture.cfg->channel[0].lec_control);
	fixture_free(&fixture);
}

/*
 * The integer readers warn on the same terms as the bool reader. A present
 * value that the parser refuses costs one warning, and then takes the schema
 * default. Two readers take an unset sentinel of a fallback chain instead:
 * opt_uint0 and the codec priority. They stay silent, because they have no
 * default to announce.
 */
static void int_warn_tests(void)
{
	struct cfg_fixture fixture;

	CASE("a malformed int warns once and takes the schema default");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(
		"\toption call_fwd_no_answer_timeout 'broken'\n"));
	CHECK_UINT(fixture.cfg->channel[0].call_fwd_no_answer_timeout, 15);
	CHECK_UINT(warn_capture_count(
		"voip call_fwd_no_answer_timeout value 'broken' is not a number"), 1);
	fixture_free(&fixture);

	CASE("a valid or absent int warns not at all");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(
		"\toption call_fwd_no_answer_timeout '22'\n"
		"\toption start_digit_timer '11'\n"));
	CHECK_UINT(fixture.cfg->channel[0].call_fwd_no_answer_timeout, 22);
	CHECK_UINT(warn_capture_count("is not a number"), 0);
	fixture_free(&fixture);

	CASE("a channel options_interval above the port range loads unclamped");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(
		"\toption options_interval '100000'\n"));
	CHECK_UINT(fixture.cfg->channel[0].options_interval, 100000);
	CHECK_UINT(warn_capture_count("options_interval"), 0);
	fixture_free(&fixture);

	CASE("the sentinel readers stay silent on text they cannot read");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config channel 'channel1'\n\toption id '0'\n"
		"\toption sip_registrar_port 'broken'\n"
		"config codec 'quiet_codec'\n\toption priority 'broken'\n"));
	CHECK_UINT(fixture.cfg->channel[0].sip_registrar_port, 0);
	CHECK_UINT(fixture.cfg->codec[0].priority, UINT_MAX);
	CHECK_UINT(warn_capture_count("is not a number"), 0);
	fixture_free(&fixture);

	CASE("a malformed alias primary warns under ITS OWN name");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(
		"\toption sip_register_retry 'broken'\n"
		"\toption sip_rereg_delta_exp_time '19'\n"));
	CHECK_UINT(warn_capture_count(
		"voip sip_register_retry value 'broken' is not a number"), 1);
	fixture_free(&fixture);

	CASE("and the alias still fills the value in");
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(
		"\toption sip_register_retry 'broken'\n"
		"\toption sip_rereg_delta_exp_time '19'\n"));
	CHECK_UINT(fixture.cfg->channel[0].sip_register_retry, 19);
	fixture_free(&fixture);

	CASE("a valid primary leaves a malformed alias unread and silent");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(
		"\toption sip_register_retry '17'\n"
		"\toption sip_rereg_delta_exp_time 'broken'\n"));
	CHECK_UINT(fixture.cfg->channel[0].sip_register_retry, 17);
	CHECK_UINT(warn_capture_count("is not a number"), 0);
	fixture_free(&fixture);

	CASE("both halves malformed warn once each and take the default");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(
		"\toption sip_register_retry 'broken'\n"
		"\toption sip_rereg_delta_exp_time 'alsobroken'\n"));
	CHECK_UINT(fixture.cfg->channel[0].sip_register_retry, 5);
	CHECK_UINT(warn_capture_count("voip sip_register_retry value 'broken'"), 1);
	fixture_free(&fixture);

	CASE("and the second warning names the alias, not the canonical option");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(
		"\toption sip_register_retry 'broken'\n"
		"\toption sip_rereg_delta_exp_time 'alsobroken'\n"));
	CHECK_UINT(warn_capture_count(
		"voip sip_rereg_delta_exp_time value 'alsobroken' is not a number"), 1);
	fixture_free(&fixture);
	warn_capture_end();
}

/* the loaded canceller and loss plan reach the diagnostic dump, which is where
 * the presence of the two optional values can be read back */
static void lec_dump_tests(void)
{
	enum {
		DUMP_CHANNEL,
		DUMP_MAX,
	};
	enum {
		CHANNEL_LEC_CONTROL_SET,
		CHANNEL_LEC_CONTROL,
		CHANNEL_LEC_ENABLE,
		CHANNEL_LEC_NLP_SET,
		CHANNEL_LEC_NLP,
		CHANNEL_LEC_TONE_LATCH,
		CHANNEL_LEC_TONE_FORCE,
		CHANNEL_HALF_DB_RX,
		CHANNEL_HALF_DB_TX,
		CHANNEL_MAX,
	};
	static const struct blobmsg_policy dump_policy[DUMP_MAX] = {
		[DUMP_CHANNEL] = { .name = "channel", .type = BLOBMSG_TYPE_ARRAY },
	};
	static const struct blobmsg_policy channel_policy[CHANNEL_MAX] = {
		[CHANNEL_LEC_CONTROL_SET] = {
			.name = "lec_control_set",
			.type = BLOBMSG_TYPE_BOOL,
		},
		[CHANNEL_LEC_CONTROL] = {
			.name = "lec_control",
			.type = BLOBMSG_TYPE_BOOL,
		},
		[CHANNEL_LEC_ENABLE] = {
			.name = "lec_enable",
			.type = BLOBMSG_TYPE_BOOL,
		},
		[CHANNEL_LEC_NLP_SET] = {
			.name = "lec_nlp_set",
			.type = BLOBMSG_TYPE_BOOL,
		},
		[CHANNEL_LEC_NLP] = { .name = "lec_nlp", .type = BLOBMSG_TYPE_BOOL },
		[CHANNEL_LEC_TONE_LATCH] = {
			.name = "lec_tone_latch",
			.type = BLOBMSG_TYPE_BOOL,
		},
		[CHANNEL_LEC_TONE_FORCE] = {
			.name = "lec_tone_force_disable",
			.type = BLOBMSG_TYPE_BOOL,
		},
		[CHANNEL_HALF_DB_RX] = {
			.name = "half_db_rx_loss",
			.type = BLOBMSG_TYPE_BOOL,
		},
		[CHANNEL_HALF_DB_TX] = {
			.name = "half_db_tx_loss",
			.type = BLOBMSG_TYPE_BOOL,
		},
	};
	struct blob_attr *dump_fields[DUMP_MAX] = { 0 };
	struct blob_attr *channel_fields[CHANNEL_MAX] = { 0 };
	struct blob_attr *channel_entry;
	struct cfg_fixture fixture;
	struct blob_buf buf = { 0 };

	CASE("the dump carries the canceller values the config named");
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(
		"\toption lec_control '1'\n"
		"\toption lec_enable '0'\n"
		"\toption lec_nlp '0'\n"
		"\toption lec_tone_latch '1'\n"
		"\toption lec_tone_force_disable '1'\n"
		"\toption half_db_rx_loss '1'\n"));
	blobmsg_buf_init(&buf);
	CHECK(buf.head != NULL);
	if (!buf.head) {
		blob_buf_free(&buf);
		fixture_free(&fixture);
		return;
	}

	cfg_dump(fixture.cfg, &buf);
	blobmsg_parse_attr(dump_policy, DUMP_MAX, dump_fields, buf.head);
	channel_entry = dump_fields[DUMP_CHANNEL] ?
		(struct blob_attr *)blobmsg_data(dump_fields[DUMP_CHANNEL]) : NULL;
	if (channel_entry)
		blobmsg_parse_attr(channel_policy, CHANNEL_MAX, channel_fields,
				   channel_entry);
	CHECK(channel_fields[CHANNEL_LEC_CONTROL_SET] != NULL);
	CHECK(channel_fields[CHANNEL_LEC_CONTROL] != NULL);
	CHECK(channel_fields[CHANNEL_LEC_ENABLE] != NULL);
	CHECK(channel_fields[CHANNEL_LEC_NLP_SET] != NULL);
	CHECK(channel_fields[CHANNEL_LEC_NLP] != NULL);
	CHECK(channel_fields[CHANNEL_LEC_TONE_LATCH] != NULL);
	CHECK(channel_fields[CHANNEL_LEC_TONE_FORCE] != NULL);
	CHECK(channel_fields[CHANNEL_HALF_DB_RX] != NULL);
	CHECK(channel_fields[CHANNEL_HALF_DB_TX] != NULL);
	if (channel_fields[CHANNEL_LEC_CONTROL])
		CHECK(blobmsg_get_bool(channel_fields[CHANNEL_LEC_CONTROL]));
	if (channel_fields[CHANNEL_LEC_ENABLE])
		CHECK(!blobmsg_get_bool(channel_fields[CHANNEL_LEC_ENABLE]));
	if (channel_fields[CHANNEL_LEC_NLP])
		CHECK(!blobmsg_get_bool(channel_fields[CHANNEL_LEC_NLP]));
	if (channel_fields[CHANNEL_LEC_TONE_LATCH])
		CHECK(blobmsg_get_bool(channel_fields[CHANNEL_LEC_TONE_LATCH]));
	if (channel_fields[CHANNEL_LEC_TONE_FORCE])
		CHECK(blobmsg_get_bool(channel_fields[CHANNEL_LEC_TONE_FORCE]));
	if (channel_fields[CHANNEL_HALF_DB_RX])
		CHECK(blobmsg_get_bool(channel_fields[CHANNEL_HALF_DB_RX]));
	if (channel_fields[CHANNEL_HALF_DB_TX])
		CHECK(!blobmsg_get_bool(channel_fields[CHANNEL_HALF_DB_TX]));

	blob_buf_free(&buf);
	fixture_free(&fixture);

	/* an absent optional value has nothing to report, which is what tells a
	 * reader that the daemon policy is the one in force */
	CASE("an absent optional value is reported as absent, not as zero");
	memset(dump_fields, 0, sizeof(dump_fields));
	memset(channel_fields, 0, sizeof(channel_fields));
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(""));
	blobmsg_buf_init(&buf);
	CHECK(buf.head != NULL);
	if (!buf.head) {
		blob_buf_free(&buf);
		fixture_free(&fixture);
		return;
	}

	cfg_dump(fixture.cfg, &buf);
	blobmsg_parse_attr(dump_policy, DUMP_MAX, dump_fields, buf.head);
	channel_entry = dump_fields[DUMP_CHANNEL] ?
		(struct blob_attr *)blobmsg_data(dump_fields[DUMP_CHANNEL]) : NULL;
	if (channel_entry)
		blobmsg_parse_attr(channel_policy, CHANNEL_MAX, channel_fields,
				   channel_entry);
	CHECK(channel_fields[CHANNEL_LEC_CONTROL_SET] != NULL);
	CHECK(channel_fields[CHANNEL_LEC_CONTROL] == NULL);
	CHECK(channel_fields[CHANNEL_LEC_NLP_SET] != NULL);
	CHECK(channel_fields[CHANNEL_LEC_NLP] == NULL);
	if (channel_fields[CHANNEL_LEC_CONTROL_SET])
		CHECK(!blobmsg_get_bool(channel_fields[CHANNEL_LEC_CONTROL_SET]));
	if (channel_fields[CHANNEL_LEC_ENABLE])
		CHECK(blobmsg_get_bool(channel_fields[CHANNEL_LEC_ENABLE]));

	/* the tone options carry no presence flag, so the dump reports both
	 * rows whatever the config named, the lec_enable shape */
	CASE("and both tone rows stand at 0, because they are unconditional");
	CHECK(channel_fields[CHANNEL_LEC_TONE_LATCH] != NULL);
	CHECK(channel_fields[CHANNEL_LEC_TONE_FORCE] != NULL);
	if (channel_fields[CHANNEL_LEC_TONE_LATCH])
		CHECK(!blobmsg_get_bool(channel_fields[CHANNEL_LEC_TONE_LATCH]));
	if (channel_fields[CHANNEL_LEC_TONE_FORCE])
		CHECK(!blobmsg_get_bool(channel_fields[CHANNEL_LEC_TONE_FORCE]));

	blob_buf_free(&buf);
	fixture_free(&fixture);
}

static void ptime_digit_tests(void)
{
	struct cfg_fixture fixture;
	char decoded[CFG_DIGIT_MAP_MAX];
	char encoded[B64_ENCODE_LEN(sizeof(decoded))];
	char option[512];
	int encoded_len;

	CASE("ptime 40 clamps to 30");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption default_packetization_rate '40'\n"));
	CHECK_UINT(fixture.cfg->channel[0].ptime_ms, 30);
	fixture_free(&fixture);

	CASE("ptime 10 passes through");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption default_packetization_rate '10'\n"));
	CHECK_UINT(fixture.cfg->channel[0].ptime_ms, 10);
	fixture_free(&fixture);

	CASE("ptime 30 passes through");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption default_packetization_rate '30'\n"));
	CHECK_UINT(fixture.cfg->channel[0].ptime_ms, 30);
	fixture_free(&fixture);

	CASE("G726 payload type 129 loads");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption g726_ptype '129'\n"));
	CHECK_UINT(fixture.cfg->channel[0].g726_ptype, 129);
	fixture_free(&fixture);

	CASE("valid custom digit map decodes");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption digit_map_base64 'MTJ4LlQ='\n"));
	CHECK_CSTR(fixture.cfg->channel[0].digit_map, "12x.T");
	fixture_free(&fixture);

	CASE("invalid digit map uses default");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption digit_map_base64 '!!!!'\n"));
	CHECK_CSTR(fixture.cfg->channel[0].digit_map, "*xx.T|x.T");
	fixture_free(&fixture);

	CASE("empty digit map uses default");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption digit_map_base64 ''\n"));
	CHECK_CSTR(fixture.cfg->channel[0].digit_map, "*xx.T|x.T");
	fixture_free(&fixture);

	CASE("digit map with embedded NUL uses default");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption digit_map_base64 'MTIAeA=='\n"));
	CHECK_CSTR(fixture.cfg->channel[0].digit_map, "*xx.T|x.T");
	fixture_free(&fixture);

	memset(decoded, 'x', sizeof(decoded));
	CASE("255 byte digit map decodes");
	encoded_len = b64_encode(decoded, CFG_DIGIT_MAP_MAX - 1, encoded,
				 sizeof(encoded));
	CHECK(encoded_len > 0);
	if (encoded_len <= 0)
		return;
	encoded[encoded_len] = '\0';
	snprintf(option, sizeof(option), "\toption digit_map_base64 '%s'\n", encoded);
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(option));
	CHECK_UINT(strlen(fixture.cfg->channel[0].digit_map), CFG_DIGIT_MAP_MAX - 1);
	CHECK_UINT(strspn(fixture.cfg->channel[0].digit_map, "x"), CFG_DIGIT_MAP_MAX - 1);
	fixture_free(&fixture);

	CASE("256 byte digit map uses default");
	encoded_len = b64_encode(decoded, sizeof(decoded), encoded, sizeof(encoded));
	CHECK(encoded_len > 0);
	if (encoded_len <= 0)
		return;
	encoded[encoded_len] = '\0';
	snprintf(option, sizeof(option), "\toption digit_map_base64 '%s'\n", encoded);
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(option));
	CHECK_CSTR(fixture.cfg->channel[0].digit_map, "*xx.T|x.T");
	fixture_free(&fixture);
}

/*
 * The base64 of a field digit map, decoded:
 * "[2-9]11|[2-9]xxxxxxxxx|1[2-9]xxxxxxxxx|*55|*56|*57|*69|*72|*73|*81|*90|*98
 * |xx.T|[*#][*#x].T|xx.#". One constant: `digit_map_warn_tests()` and the
 * field push fixtures below all need the same map on the wire.
 */
#define FIELD_DIGIT_MAP_B64 \
	"WzItOV0xMXxbMi05XXh4eHh4eHh4eHwxWz" \
	"ItOV14eHh4eHh4eHh8KjU1fCo1NnwqNTd8KjY5fCo3MnwqNzN8KjgxfCo5MHwq" \
	"OTh8eHguVHxbKiNdWyojeF0uVHx4eC4j"

/*
 * The report for a map that decodes but does not compile. When the configured
 * map does not compile, line/line.c falls back to the default map, per line and
 * per reload. The loader is therefore the one place that runs once per load
 * and can name the section.
 */
static void digit_map_warn_tests(void)
{
	/* base64 of "N5", which the grammar refuses */
	static const char refused[] = "\toption digit_map_base64 'TjU='\n";
	/* base64 of "x.T\nforged", a map whose line feed would start a line of
	 * its own in the log */
	static const char control[] =
		"\toption digit_map_base64 'eC5UCmZvcmdlZA=='\n";
	static const char field_map_opt[] =
		"\toption digit_map_base64 '" FIELD_DIGIT_MAP_B64 "'\n";
	struct cfg_fixture fixture;

	CASE("digit map: a refused map warns once and names the section");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(refused));
	CHECK_UINT(warn_capture_count("voip.channel1 digit_map_base64 decodes "
				      "to 'N5', which is not a valid digit "
				      "map, using '*xx.T|x.T'"), 1);
	warn_capture_end();
	/* the decoded text travels on, and line/line.c chooses the fallback */
	CHECK_CSTR(fixture.cfg->channel[0].digit_map, "N5");
	fixture_free(&fixture);

	CASE("digit map: a map that compiles warns not at all");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(field_map_opt));
	CHECK_UINT(warn_capture_count("is not a valid digit map"), 0);
	warn_capture_end();
	CHECK_CSTR(fixture.cfg->channel[0].digit_map,
		   "[2-9]11|[2-9]xxxxxxxxx|1[2-9]xxxxxxxxx|*55|*56|*57|*69|*72"
		   "|*73|*81|*90|*98|xx.T|[*#][*#x].T|xx.#");
	fixture_free(&fixture);

	/* the decoder stops it, so the text of a forged line never reaches the
	 * log the compile report writes */
	CASE("digit map: a decoded control byte is refused by the decoder and "
	     "never printed");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(control));
	CHECK_UINT(warn_capture_count("voip digit_map_base64 is invalid, using "
				      "decoded default"), 1);
	CHECK_UINT(warn_capture_count("is not a valid digit map"), 0);
	CHECK_UINT(warn_capture_count("forged"), 0);
	warn_capture_end();
	CHECK_CSTR(fixture.cfg->channel[0].digit_map, "*xx.T|x.T");
	fixture_free(&fixture);
}

static void dtmf_tests(void)
{
	static const struct {
		const char *value;
		enum cfg_dtmf_mode mode;
	} examples[] = {
		{ "sipinfo", CFG_DTMF_INFO },
		{ "sip_info", CFG_DTMF_INFO },
		{ "info", CFG_DTMF_INFO },
		{ "rfc2833", CFG_DTMF_RFC2833 },
		{ "broken", CFG_DTMF_INBAND },
	};
	static const char *const names[] = {
		"dtmf sipinfo maps to info",
		"dtmf sip_info maps to info",
		"dtmf info maps to info",
		"dtmf rfc2833 maps to rfc2833",
		"bad dtmf value uses inband",
	};
	struct cfg_fixture fixture;
	char option[128];
	unsigned int i;

	for (i = 0; i < sizeof(examples) / sizeof(examples[0]); i++) {
		CASE(names[i]);
		snprintf(option, sizeof(option), "\toption sip_dtmf_mode '%s'\n",
			 examples[i].value);
		FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(option));
		CHECK(fixture.cfg->channel[0].dtmf_mode == examples[i].mode);
		fixture_free(&fixture);
	}
}

static void alias_tests(void)
{
	struct cfg_fixture fixture;

	CASE("interdigit long alias alone");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption interdigit_long '21'\n"));
	CHECK_UINT(fixture.cfg->channel[0].interdigit_long_s, 21);
	fixture_free(&fixture);

	CASE("interdigit long canonical alone");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption interdigit_timeout_long '22'\n"));
	CHECK_UINT(fixture.cfg->channel[0].interdigit_long_s, 22);
	fixture_free(&fixture);

	CASE("interdigit long canonical wins");
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(
		"\toption interdigit_long '21'\n"
		"\toption interdigit_timeout_long '22'\n"));
	CHECK_UINT(fixture.cfg->channel[0].interdigit_long_s, 22);
	fixture_free(&fixture);

	CASE("cancel cw off alias enables");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption cancel_cw_off_exten '*71#'\n"));
	CHECK_CSTR(fixture.cfg->channel[0].cw_enable_code, "*71#");
	fixture_free(&fixture);

	CASE("cancel cw on alias disables");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption cancel_cw_on_exten '#71#'\n"));
	CHECK_CSTR(fixture.cfg->channel[0].cw_disable_code, "#71#");
	fixture_free(&fixture);

	CASE("call waiting enable alias wins");
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(
		"\toption cancel_cw_off_exten '*71#'\n"
		"\toption call_waiting_enable_exten '*72#'\n"));
	CHECK_CSTR(fixture.cfg->channel[0].cw_enable_code, "*71#");
	fixture_free(&fixture);

	CASE("call waiting disable alias wins");
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(
		"\toption cancel_cw_on_exten '#71#'\n"
		"\toption call_waiting_disable_exten '#72#'\n"));
	CHECK_CSTR(fixture.cfg->channel[0].cw_disable_code, "#71#");
	fixture_free(&fixture);

	CASE("empty call waiting canonical clears default");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\tlist call_waiting_enable_exten ''\n"));
	CHECK_CSTR(fixture.cfg->channel[0].cw_enable_code, "");
	fixture_free(&fixture);

	CASE("empty call waiting alias clears canonical");
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(
		"\tlist cancel_cw_on_exten ''\n"
		"\toption call_waiting_disable_exten '#72#'\n"));
	CHECK_CSTR(fixture.cfg->channel[0].cw_disable_code, "");
	fixture_free(&fixture);
}

static void interdigit_tests(void)
{
	struct cfg_fixture fixture;

	CASE("interdigit milliseconds convert 4000 to 4");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption interdigit_timeout '4000'\n"));
	CHECK_UINT(fixture.cfg->channel[0].interdigit_s, 4);
	fixture_free(&fixture);

	CASE("interdigit boundary 100 stays 100");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption interdigit_timeout '100'\n"));
	CHECK_UINT(fixture.cfg->channel[0].interdigit_s, 100);
	fixture_free(&fixture);

	CASE("interdigit milliseconds convert 101 to 0");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption interdigit_timeout '101'\n"));
	CHECK_UINT(fixture.cfg->channel[0].interdigit_s, 0);
	fixture_free(&fixture);

	CASE("interdigit long alias clamps to INT_MAX");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption interdigit_long '4000000000'\n"));
	CHECK_UINT(fixture.cfg->channel[0].interdigit_long_s, INT_MAX);
	fixture_free(&fixture);

	CASE("start digit timer clamps to INT_MAX");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption start_digit_timer '4294967295'\n"));
	CHECK_UINT(fixture.cfg->channel[0].start_digit_s, INT_MAX);
	fixture_free(&fixture);
}

static void presence_session_tests(void)
{
	struct cfg_fixture fixture;

	CASE("call waiting local absent");
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(""));
	CHECK(!fixture.cfg->channel[0].call_waiting_local_set);
	fixture_free(&fixture);

	CASE("call waiting local zero present");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption call_waiting_local '0'\n"));
	CHECK(fixture.cfg->channel[0].call_waiting_local_set);
	CHECK(!fixture.cfg->channel[0].call_waiting_local);
	fixture_free(&fixture);

	CASE("call waiting local one present");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption call_waiting_local '1'\n"));
	CHECK(fixture.cfg->channel[0].call_waiting_local_set);
	CHECK(fixture.cfg->channel[0].call_waiting_local);
	fixture_free(&fixture);

	CASE("invalid call waiting local value stays unset");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption call_waiting_local 'broken'\n"));
	CHECK(!fixture.cfg->channel[0].call_waiting_local_set);
	fixture_free(&fixture);

	CASE("session options absent stay zero");
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(""));
	CHECK_UINT(fixture.cfg->channel[0].session_max_expires, 0);
	CHECK_UINT(fixture.cfg->channel[0].session_min_interval, 0);
	CHECK_UINT(fixture.cfg->channel[0].timer_c_ms, 0);
	fixture_free(&fixture);

	CASE("session options load");
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(
		"\toption sip_session_max_expires '900'\n"
		"\toption sip_session_min_interval '90'\n"
		"\toption sip_session_refresher 'unspecified'\n"
		"\toption sip_timer_c '200000'\n"));
	CHECK_UINT(fixture.cfg->channel[0].session_max_expires, 900);
	CHECK_UINT(fixture.cfg->channel[0].session_min_interval, 90);
	CHECK(fixture.cfg->channel[0].session_refresher == CFG_REFRESH_UNSPEC);
	CHECK_UINT(fixture.cfg->channel[0].timer_c_ms, 200000);
	fixture_free(&fixture);

	CASE("timer c below minimum clamps");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption sip_timer_c '1'\n"));
	CHECK_UINT(fixture.cfg->channel[0].timer_c_ms, 180001);
	fixture_free(&fixture);

	CASE("timer c above INT_MAX clamps");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption sip_timer_c '4000000000'\n"));
	CHECK_UINT(fixture.cfg->channel[0].timer_c_ms, INT_MAX);
	fixture_free(&fixture);

	CASE("session refresher explicit uac");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption sip_session_refresher 'uac'\n"));
	CHECK(fixture.cfg->channel[0].session_refresher == CFG_REFRESH_UAC);
	fixture_free(&fixture);

	CASE("invalid session refresher uses uac");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption sip_session_refresher 'invalid'\n"));
	CHECK(fixture.cfg->channel[0].session_refresher == CFG_REFRESH_UAC);
	fixture_free(&fixture);
}

static void codec_tests(void)
{
	const char ordered[] =
		"config codec 'two'\n"
		"\toption tr104_id '2'\n"
		"\toption priority '2'\n"
		"\toption default_priority '2'\n"
		"config codec 'one'\n"
		"\toption tr104_id '1'\n"
		"\toption priority '1'\n"
		"\toption default_priority '1'\n"
		"config codec 'three'\n"
		"\toption tr104_id '3'\n"
		"\toption priority '3'\n"
		"\toption default_priority '3'\n";
	const char tied[] =
		"config codec 'late'\n"
		"\toption tr104_id '1'\n"
		"\toption priority '2'\n"
		"\toption default_priority '4'\n"
		"config codec 'early'\n"
		"\toption tr104_id '2'\n"
		"\toption priority '2'\n"
		"\toption default_priority '1'\n";
	const char twelve_channels[] =
		"config codec\n\toption tr104_id '6'\n\toption priority '12'\n"
		"\toption channel '2'\n"
		"config codec\n\toption tr104_id '5'\n\toption priority '11'\n"
		"\toption channel '2'\n"
		"config codec\n\toption tr104_id '4'\n\toption priority '10'\n"
		"\toption channel '2'\n"
		"config codec\n\toption tr104_id '3'\n\toption priority '9'\n"
		"\toption channel '2'\n"
		"config codec\n\toption tr104_id '2'\n\toption priority '8'\n"
		"\toption channel '2'\n"
		"config codec\n\toption tr104_id '1'\n\toption priority '7'\n"
		"\toption channel '2'\n"
		"config codec\n\toption tr104_id '6'\n\toption priority '6'\n"
		"\toption channel '1'\n"
		"config codec\n\toption tr104_id '5'\n\toption priority '5'\n"
		"\toption channel '1'\n"
		"config codec\n\toption tr104_id '4'\n\toption priority '4'\n"
		"\toption channel '1'\n"
		"config codec\n\toption tr104_id '3'\n\toption priority '3'\n"
		"\toption channel '1'\n"
		"config codec\n\toption tr104_id '2'\n\toption priority '2'\n"
		"\toption channel '1'\n"
		"config codec\n\toption tr104_id '1'\n\toption priority '1'\n"
		"\toption channel '1'\n";
	const char seventeen_tail_drop[] =
		"config codec\n\toption priority '17'\n"
		"config codec\n\toption priority '16'\n"
		"config codec\n\toption priority '15'\n"
		"config codec\n\toption priority '14'\n"
		"config codec\n\toption priority '13'\n"
		"config codec\n\toption priority '12'\n"
		"config codec\n\toption priority '11'\n"
		"config codec\n\toption priority '10'\n"
		"config codec\n\toption priority '9'\n"
		"config codec\n\toption priority '8'\n"
		"config codec\n\toption priority '7'\n"
		"config codec\n\toption priority '6'\n"
		"config codec\n\toption priority '5'\n"
		"config codec\n\toption priority '4'\n"
		"config codec\n\toption priority '3'\n"
		"config codec\n\toption priority '2'\n"
		"config codec\n\toption priority '1'\n";
	const char seventeen_insert_refusal[] =
		"config codec\n\toption tr104_id '1'\n\toption priority '1'\n"
		"config codec\n\toption tr104_id '1'\n\toption priority '2'\n"
		"config codec\n\toption tr104_id '1'\n\toption priority '3'\n"
		"config codec\n\toption tr104_id '1'\n\toption priority '4'\n"
		"config codec\n\toption tr104_id '1'\n\toption priority '5'\n"
		"config codec\n\toption tr104_id '1'\n\toption priority '6'\n"
		"config codec\n\toption tr104_id '1'\n\toption priority '7'\n"
		"config codec\n\toption tr104_id '1'\n\toption priority '8'\n"
		"config codec\n\toption tr104_id '1'\n\toption priority '9'\n"
		"config codec\n\toption tr104_id '1'\n\toption priority '10'\n"
		"config codec\n\toption tr104_id '1'\n\toption priority '11'\n"
		"config codec\n\toption tr104_id '1'\n\toption priority '12'\n"
		"config codec\n\toption tr104_id '1'\n\toption priority '13'\n"
		"config codec\n\toption tr104_id '1'\n\toption priority '14'\n"
		"config codec\n\toption tr104_id '1'\n\toption priority '15'\n"
		"config codec\n\toption tr104_id '1'\n\toption priority '16'\n"
		"config codec\n\toption tr104_id '4'\n\toption priority '17'\n"
		"\toption default_priority '17'\n\toption enabled '0'\n";
	const char missing_priorities[] =
		"config codec 'missing_first'\n\toption tr104_id '1'\n"
		"config codec 'priority_after'\n\toption tr104_id '2'\n"
		"\toption priority '1'\n\toption default_priority '1'\n"
		"config codec 'priority_before'\n\toption tr104_id '3'\n"
		"\toption priority '2'\n\toption default_priority '2'\n"
		"config codec 'bad_after'\n\toption tr104_id '4'\n"
		"\toption priority 'bad'\n\toption default_priority 'bad'\n";
	const char missing_default_priorities[] =
		"config codec 'missing_first'\n\toption tr104_id '1'\n"
		"\toption priority '3'\n"
		"config codec 'default_after'\n\toption tr104_id '2'\n"
		"\toption priority '3'\n\toption default_priority '1'\n"
		"config codec 'default_before'\n\toption tr104_id '3'\n"
		"\toption priority '4'\n\toption default_priority '1'\n"
		"config codec 'bad_after'\n\toption tr104_id '4'\n"
		"\toption priority '4'\n\toption default_priority 'bad'\n";
	struct cfg_fixture fixture;
	unsigned int i;

	CASE("codec count loads");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(ordered));
	CHECK_UINT(fixture.cfg->codecs, 3);

	CASE("codec priority first");
	CHECK_UINT(fixture.cfg->codec[0].tr104_id, 1);
	CHECK_UINT(fixture.cfg->codec[0].priority, 1);

	CASE("codec priority middle");
	CHECK_UINT(fixture.cfg->codec[1].tr104_id, 2);
	CHECK_UINT(fixture.cfg->codec[1].priority, 2);

	CASE("codec priority last");
	CHECK_UINT(fixture.cfg->codec[2].tr104_id, 3);
	CHECK_UINT(fixture.cfg->codec[2].priority, 3);
	fixture_free(&fixture);

	CASE("codec default priority breaks tie");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(tied));
	CHECK_UINT(fixture.cfg->codec[0].tr104_id, 2);
	CHECK_UINT(fixture.cfg->codec[0].default_priority, 1);
	CHECK_UINT(fixture.cfg->codec[1].tr104_id, 1);
	CHECK_UINT(fixture.cfg->codec[1].default_priority, 4);
	fixture_free(&fixture);

	CASE("disabled codec remains in dense array");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config codec\n\toption tr104_id '1'\n\toption priority '1'\n"
		"\toption enabled '0'\n"));
	CHECK_UINT(fixture.cfg->codecs, 1);
	CHECK(!fixture.cfg->codec[0].enabled);
	fixture_free(&fixture);

	CASE("bad codec tr104 id becomes zero");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config codec\n\toption tr104_id '7'\n\toption priority '1'\n"));
	CHECK_UINT(fixture.cfg->codecs, 1);
	CHECK_UINT(fixture.cfg->codec[0].tr104_id, 0);
	fixture_free(&fixture);

	CASE("the closed codec enum accepts G.722 and Opus");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config codec 'g722'\n\toption tr104_id '5'\n\toption priority '1'\n"
		"config codec 'opus'\n\toption tr104_id '6'\n\toption priority '2'\n"));
	CHECK_UINT(fixture.cfg->codecs, 2);
	CHECK_UINT(fixture.cfg->codec[0].tr104_id, 5);
	CHECK_UINT(fixture.cfg->codec[1].tr104_id, 6);
	CHECK_UINT(fixture.cfg->codec[0].bitrate, 0);
	CHECK(fixture.cfg->codec[0].fec);
	fixture_free(&fixture);

	CASE("Opus zero parameters select the library defaults");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config codec 'opus'\n\toption tr104_id '6'\n"
		"\toption bitrate '0'\n\toption complexity '0'\n\toption fec '0'\n"));
	CHECK_UINT(fixture.cfg->codec[0].bitrate, 0);
	CHECK_UINT(fixture.cfg->codec[0].complexity, 0);
	CHECK(!fixture.cfg->codec[0].fec);
	fixture_free(&fixture);

	CASE("Opus parameters clamp to the codec bounds");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config codec 'opus'\n\toption tr104_id '6'\n"
		"\toption bitrate '512001'\n\toption complexity '11'\n"));
	CHECK_UINT(fixture.cfg->codec[0].bitrate, 512000);
	CHECK_UINT(fixture.cfg->codec[0].complexity, 10);
	fixture_free(&fixture);

	CASE("twelve codecs across two channels retain their bindings");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(twelve_channels));
	CHECK_UINT(fixture.cfg->codecs, 12);
	for (i = 0; i < fixture.cfg->codecs; i++) {
		CHECK_UINT(fixture.cfg->codec[i].priority, i + 1);
		CHECK_UINT(fixture.cfg->codec[i].channel, i / 6 + 1);
		CHECK_UINT(fixture.cfg->codec[i].tr104_id, i % 6 + 1);
	}
	fixture_free(&fixture);

	CASE("seventeen codecs tail-drop the lowest priority");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(seventeen_tail_drop));
	CHECK_UINT(fixture.cfg->codecs, 16);
	CHECK_UINT(fixture.cfg->codecs, CFG_CODEC_MAX);
	for (i = 0; i < CFG_CODEC_MAX; i++)
		CHECK_UINT(fixture.cfg->codec[i].priority, i + 1);
	CHECK_UINT(warn_capture_count("codec section limit"), 1);
	fixture_free(&fixture);

	CASE("seventeenth worst codec is refused whole");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(seventeen_insert_refusal));
	CHECK_UINT(fixture.cfg->codecs, 16);
	CHECK_UINT(fixture.cfg->codecs, CFG_CODEC_MAX);
	for (i = 0; i < CFG_CODEC_MAX; i++) {
		CHECK_UINT(fixture.cfg->codec[i].priority, i + 1);
		CHECK_UINT(fixture.cfg->codec[i].tr104_id, 1);
		CHECK(fixture.cfg->codec[i].enabled);
	}
	CHECK_UINT(warn_capture_count("codec section limit"), 1);
	fixture_free(&fixture);
	warn_capture_end();

	CASE("prioritized codecs beat missing priorities in either order");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(missing_priorities));
	CHECK_UINT(fixture.cfg->codec[0].tr104_id, 2);
	CHECK_UINT(fixture.cfg->codec[1].tr104_id, 3);
	CHECK_UINT(fixture.cfg->codec[2].priority, UINT_MAX);
	CHECK_UINT(fixture.cfg->codec[3].priority, UINT_MAX);
	CHECK_UINT(fixture.cfg->codec[2].default_priority, UINT_MAX);
	CHECK_UINT(fixture.cfg->codec[3].default_priority, UINT_MAX);
	fixture_free(&fixture);

	CASE("default priorities beat missing tie breakers in either order");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(missing_default_priorities));
	CHECK_UINT(fixture.cfg->codec[0].tr104_id, 2);
	CHECK_UINT(fixture.cfg->codec[1].tr104_id, 1);
	CHECK_UINT(fixture.cfg->codec[2].tr104_id, 3);
	CHECK_UINT(fixture.cfg->codec[3].tr104_id, 4);
	CHECK_UINT(fixture.cfg->codec[1].default_priority, UINT_MAX);
	CHECK_UINT(fixture.cfg->codec[3].default_priority, UINT_MAX);
	fixture_free(&fixture);
}

static void wideband_option_tests(void)
{
	struct cfg_fixture fixture;

	CASE("G.722 keeps static payload type 9 or the disabled sentinel");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption g722_ptype '129'\n"));
	CHECK_UINT(fixture.cfg->channel[0].g722_ptype, 129);
	fixture_free(&fixture);
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption g722_ptype '10'\n"));
	CHECK_UINT(fixture.cfg->channel[0].g722_ptype, 9);
	fixture_free(&fixture);

	CASE("Opus accepts the dynamic payload range and the disabled sentinel");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption opus_ptype '96'\n"));
	CHECK_UINT(fixture.cfg->channel[0].opus_ptype, 96);
	fixture_free(&fixture);
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption opus_ptype '127'\n"));
	CHECK_UINT(fixture.cfg->channel[0].opus_ptype, 127);
	fixture_free(&fixture);
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption opus_ptype '129'\n"));
	CHECK_UINT(fixture.cfg->channel[0].opus_ptype, 129);
	fixture_free(&fixture);
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption opus_ptype '95'\n"));
	CHECK_UINT(fixture.cfg->channel[0].opus_ptype, 96);
	fixture_free(&fixture);

	CASE("Opus refuses the telephone-event payload type");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption opus_ptype '101'\n"));
	CHECK_UINT(fixture.cfg->channel[0].opus_ptype, 96);
	fixture_free(&fixture);

	CASE("an invalid Opus FEC value takes the enabled default");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config codec 'opus'\n\toption tr104_id '6'\n\toption fec '2'\n"));
	CHECK(fixture.cfg->codec[0].fec);
	fixture_free(&fixture);
}

static void port_tests(void)
{
	struct cfg_fixture fixture;

	CASE("RTP port range loads");
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(
		"\toption rtp_local_port_min '20000'\n"
		"\toption rtp_local_port_max '20008'\n"));
	CHECK_UINT(fixture.cfg->channel[0].rtp_port_min, 20000);
	CHECK_UINT(fixture.cfg->channel[0].rtp_port_max, 20008);
	fixture_free(&fixture);

	CASE("RTP minimum clamps to schema bound");
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(
		"\toption rtp_local_port_min '1'\n"
		"\toption rtp_local_port_max '20008'\n"));
	CHECK_UINT(fixture.cfg->channel[0].rtp_port_min, 1024);
	CHECK_UINT(fixture.cfg->channel[0].rtp_port_max, 20008);
	fixture_free(&fixture);

	CASE("RTP maximum clamps to schema bound");
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(
		"\toption rtp_local_port_min '20000'\n"
		"\toption rtp_local_port_max '70000'\n"));
	CHECK_UINT(fixture.cfg->channel[0].rtp_port_min, 20000);
	CHECK_UINT(fixture.cfg->channel[0].rtp_port_max, 65535);
	fixture_free(&fixture);

	CASE("RTP descending range becomes unset");
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(
		"\toption rtp_local_port_min '30000'\n"
		"\toption rtp_local_port_max '20000'\n"));
	CHECK_UINT(fixture.cfg->channel[0].rtp_port_min, 0);
	CHECK_UINT(fixture.cfg->channel[0].rtp_port_max, 0);
	fixture_free(&fixture);

	CASE("RTP minimum alone becomes unset");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption rtp_local_port_min '20000'\n"));
	CHECK_UINT(fixture.cfg->channel[0].rtp_port_min, 0);
	CHECK_UINT(fixture.cfg->channel[0].rtp_port_max, 0);
	fixture_free(&fixture);

	CASE("RTP maximum alone becomes unset");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption rtp_local_port_max '20008'\n"));
	CHECK_UINT(fixture.cfg->channel[0].rtp_port_min, 0);
	CHECK_UINT(fixture.cfg->channel[0].rtp_port_max, 0);
	fixture_free(&fixture);
}

static const char srtp_config[] =
	"config base 'base'\n"
	"\toption channels '1'\n"
	"\toption srtp_enabled '1'\n"
	"config channel 'channel1'\n"
	"\toption id '0'\n";

static const char tls_config[] =
	"config base 'base'\n"
	"\toption channels '1'\n"
	"\toption tls_cert_path 'https://certs.example.test/bundle.tgz'\n"
	"\toption tls_cert_username 'certuser'\n"
	"\toption tls_cert_password 'certpass'\n"
	"\toption tls_server_verification_enabled '1'\n"
	"\toption tls_keylog_enabled '1'\n"
	"config channel 'channel1'\n"
	"\toption id '0'\n"
	"\toption sip_port '5080'\n";

/*
 * The five base TLS options. The loader carries them, and account.c reads
 * them. The certificate fetch takes the path and the two credentials. The TLS
 * contexts take the two switches.
 */
static void tls_tests(void)
{
	char long_path[CFG_TLS_PATH_MAX * 2];
	char config[512];
	struct cfg_fixture fixture;

	CASE("the five base TLS options load");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(tls_config));
	CHECK_CSTR(fixture.cfg->tls_cert_path,
		   "https://certs.example.test/bundle.tgz");
	CHECK_CSTR(fixture.cfg->tls_cert_username, "certuser");
	CHECK_CSTR(fixture.cfg->tls_cert_password, "certpass");
	CHECK(fixture.cfg->tls_server_verification_enabled);
	CHECK(fixture.cfg->tls_keylog_enabled);
	fixture_free(&fixture);

	CASE("absent base TLS options load empty and off");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n\toption channels '1'\n"));
	CHECK_CSTR(fixture.cfg->tls_cert_path, "");
	CHECK_CSTR(fixture.cfg->tls_cert_username, "");
	CHECK_CSTR(fixture.cfg->tls_cert_password, "");
	CHECK(!fixture.cfg->tls_server_verification_enabled);
	CHECK(!fixture.cfg->tls_keylog_enabled);
	fixture_free(&fixture);

	/* opt_str() writes through snprintf, so a URL the field cannot hold is
	 * truncated and nothing warns. The bound is one byte of the field. */
	CASE("an over-long tls_cert_path truncates to the field bound");
	memset(long_path, 'a', sizeof(long_path) - 1);
	long_path[sizeof(long_path) - 1] = '\0';
	memcpy(long_path, "https://certs.example.test/",
	       strlen("https://certs.example.test/"));
	snprintf(config, sizeof(config),
		 "config base 'base'\n\toption tls_cert_path '%s'\n", long_path);
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(config));
	CHECK_UINT(strlen(fixture.cfg->tls_cert_path), CFG_TLS_PATH_MAX - 1);
	CHECK(!memcmp(fixture.cfg->tls_cert_path, long_path, CFG_TLS_PATH_MAX - 1));
	CHECK_UINT(warn_capture_count("tls_cert_path"), 0);
	fixture_free(&fixture);

	/*
	 * The placement of the five loads is the contract here. A channel
	 * count the loader cannot read is a provisioning fault, and
	 * base_parse() answers it by disabling voice and returning. The five
	 * TLS options are read ahead of that return, so the fault does not
	 * silently drop them: a TLS deployment whose channels option is broken
	 * still reports the archive and the two switches an operator
	 * configured. A load moved below the fault return fails this case.
	 */
	CASE("the channels fault leaves the TLS options loaded");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n"
		"\toption enabled '1'\n"
		"\toption channels 'abc'\n"
		"\toption tls_cert_path 'ftp://certs.example.test/bundle.tgz'\n"
		"\toption tls_cert_username 'faultuser'\n"
		"\toption tls_cert_password 'faultpass'\n"
		"\toption tls_server_verification_enabled '1'\n"
		"\toption tls_keylog_enabled '1'\n"));
	CHECK(!fixture.cfg->enabled);
	CHECK_UINT(fixture.cfg->channels, 0);
	CHECK_UINT(warn_capture_count(
		"voip.base.channels 'abc' is not a number, voice disabled"), 1);
	CHECK_CSTR(fixture.cfg->tls_cert_path, "ftp://certs.example.test/bundle.tgz");
	CHECK_CSTR(fixture.cfg->tls_cert_username, "faultuser");
	CHECK_CSTR(fixture.cfg->tls_cert_password, "faultpass");
	CHECK(fixture.cfg->tls_server_verification_enabled);
	CHECK(fixture.cfg->tls_keylog_enabled);
	fixture_free(&fixture);
	warn_capture_end();
}

/*
 * sip_port keeps the schema default 5060 (the cfg.h never-zero contract), so
 * its value cannot say whether an operator named a remote port. The presence
 * flag is what answers that, and the TLS default port rule of
 * acct_cfg_derive() in account.c is its reader.
 */
static void sip_port_presence_tests(void)
{
	struct cfg_fixture fixture;

	CASE("an explicit sip_port raises the presence flag");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption sip_port '5080'\n"));
	CHECK_UINT(fixture.cfg->channel[0].sip_port, 5080);
	CHECK(fixture.cfg->channel[0].sip_port_set);
	fixture_free(&fixture);

	CASE("an explicit sip_port of 5060 is present, not defaulted");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption sip_port '5060'\n"));
	CHECK_UINT(fixture.cfg->channel[0].sip_port, 5060);
	CHECK(fixture.cfg->channel[0].sip_port_set);
	fixture_free(&fixture);

	/* a clamped value is still a value the config named; the read-as-absent
	 * rule covers text the parser cannot read, and nothing else */
	CASE("a clamped sip_port keeps the presence flag");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption sip_port '70000'\n"));
	CHECK_UINT(fixture.cfg->channel[0].sip_port, 65535);
	CHECK(fixture.cfg->channel[0].sip_port_set);
	fixture_free(&fixture);

	CASE("an absent sip_port keeps 5060 with the flag clear");
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(""));
	CHECK_UINT(fixture.cfg->channel[0].sip_port, 5060);
	CHECK(!fixture.cfg->channel[0].sip_port_set);
	fixture_free(&fixture);

	CASE("a malformed sip_port warns once and reads as absent");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption sip_port 'broken'\n"));
	CHECK_UINT(fixture.cfg->channel[0].sip_port, 5060);
	CHECK(!fixture.cfg->channel[0].sip_port_set);
	CHECK_UINT(warn_capture_count(
		"voip sip_port value 'broken' is not a number"), 1);
	fixture_free(&fixture);
	warn_capture_end();
}

/*
 * A field push: two channels, line 2 off, no port and no domain options (DMC
 * removes an option it would otherwise send as 0 or empty), the field digit
 * map, and two options uvoice does not know (directory_number,
 * tr104_sip_client_alias). The digit map base64 is FIELD_DIGIT_MAP_B64, the
 * field map with the '[*#x]' wildcard class, which compiles because the
 * grammar accepts 'x' inside a class.
 */
static const char field_config[] =
	"config base 'base'\n"
	"\toption enabled '1'\n"
	"\toption channels '2'\n"
	"\toption local_interface 'wan'\n"
	"config channel 'channel1'\n"
	"\toption id '0'\n"
	"\toption enabled '1'\n"
	"\toption sip_proxy 'sbc-a.p2.example.com'\n"
	"\toption sip_username '15551234567'\n"
	"\toption sip_auth_username '15551234567'\n"
	"\toption sip_secret 's3cr3t1'\n"
	"\toption sip_dtmf_mode 'inband'\n"
	"\toption digit_map_base64 '" FIELD_DIGIT_MAP_B64 "'\n"
	"\toption cn_enable '0'\n"
	"\toption amwi '1'\n"
	"\toption vmwi '1'\n"
	"\toption directory_number '15551234567'\n"
	"\toption tr104_sip_client_alias 'cpe-1'\n"
	"config channel 'channel2'\n"
	"\toption id '1'\n"
	"\toption enabled '0'\n"
	"\toption sip_proxy 'sbc-a.p2.example.com'\n"
	"\toption sip_username '15551234568'\n"
	"\toption sip_auth_username '15551234568'\n"
	"\toption sip_secret 's3cr3t2'\n"
	"\toption sip_dtmf_mode 'inband'\n"
	"\toption digit_map_base64 '" FIELD_DIGIT_MAP_B64 "'\n"
	"\toption cn_enable '0'\n"
	"\toption amwi '1'\n"
	"\toption vmwi '1'\n"
	"\toption directory_number '15551234568'\n"
	"\toption tr104_sip_client_alias 'cpe-2'\n";

/*
 * The same push after a second one that moves channel 2: it comes up
 * enabled, on another proxy, and channel 1 must read the same as it did
 * before (the shape with two enabled lines).
 */
static const char field_config_moved[] =
	"config base 'base'\n"
	"\toption enabled '1'\n"
	"\toption channels '2'\n"
	"\toption local_interface 'wan'\n"
	"config channel 'channel1'\n"
	"\toption id '0'\n"
	"\toption enabled '1'\n"
	"\toption sip_proxy 'sbc-a.p2.example.com'\n"
	"\toption sip_username '15551234567'\n"
	"\toption sip_auth_username '15551234567'\n"
	"\toption sip_secret 's3cr3t1'\n"
	"\toption sip_dtmf_mode 'inband'\n"
	"\toption digit_map_base64 '" FIELD_DIGIT_MAP_B64 "'\n"
	"\toption cn_enable '0'\n"
	"\toption amwi '1'\n"
	"\toption vmwi '1'\n"
	"\toption directory_number '15551234567'\n"
	"\toption tr104_sip_client_alias 'cpe-1'\n"
	"config channel 'channel2'\n"
	"\toption id '1'\n"
	"\toption enabled '1'\n"
	"\toption sip_proxy 'sbc-b.p2.example.com'\n"
	"\toption sip_username '15551234569'\n"
	"\toption sip_auth_username '15551234569'\n"
	"\toption sip_secret 's3cr3t3'\n"
	"\toption sip_dtmf_mode 'inband'\n"
	"\toption digit_map_base64 '" FIELD_DIGIT_MAP_B64 "'\n"
	"\toption cn_enable '0'\n"
	"\toption amwi '1'\n"
	"\toption vmwi '1'\n"
	"\toption directory_number '15551234569'\n"
	"\toption tr104_sip_client_alias 'cpe-2'\n";

static void field_push_tests(void)
{
	struct cfg_fixture fixture;

	CASE("field push: the corrected push loads two channels with the proxy on both");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(field_config));
	CHECK(fixture.cfg->enabled);
	CHECK_UINT(fixture.cfg->channels, 2);
	CHECK_CSTR(fixture.cfg->local_interface, "wan");
	CHECK(fixture.cfg->channel[0].enabled);
	CHECK(!fixture.cfg->channel[1].enabled);
	CHECK_CSTR(fixture.cfg->channel[0].sip_proxy, "sbc-a.p2.example.com");
	CHECK_CSTR(fixture.cfg->channel[1].sip_proxy, "sbc-a.p2.example.com");
	fixture_free(&fixture);

	/* the fixture the corrected push actually is, not a one-off minimal
	 * section: proves the rule holds with the field push's other options
	 * in the same section, and on both channels at once */
	CASE("field push: an absent sip_port keeps 5060 with the presence flag clear");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(field_config));
	CHECK_UINT(fixture.cfg->channel[0].sip_port, 5060);
	CHECK(!fixture.cfg->channel[0].sip_port_set);
	CHECK_UINT(fixture.cfg->channel[1].sip_port, 5060);
	CHECK(!fixture.cfg->channel[1].sip_port_set);
	fixture_free(&fixture);

	CASE("field push: an empty sip_port is dropped by libuci and reads as absent");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption sip_port ''\n"));
	CHECK_UINT(fixture.cfg->channel[0].sip_port, 5060);
	CHECK(!fixture.cfg->channel[0].sip_port_set);
	fixture_free(&fixture);

	CASE("field push: a sip_port of '0' reads as absent and keeps 5060");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption sip_port '0'\n"));
	CHECK_UINT(fixture.cfg->channel[0].sip_port, 5060);
	CHECK(!fixture.cfg->channel[0].sip_port_set);
	CHECK_UINT(warn_capture_count("sip_port"), 0);
	fixture_free(&fixture);
	warn_capture_end();

	/* uint_parse() reads '00' the same as '0': still zero, still no
	 * warning, so the presence rule reads the parsed value and not the
	 * text of the option */
	CASE("field push: a sip_port of '00' reads as absent too");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption sip_port '00'\n"));
	CHECK_UINT(fixture.cfg->channel[0].sip_port, 5060);
	CHECK(!fixture.cfg->channel[0].sip_port_set);
	CHECK_UINT(warn_capture_count("sip_port"), 0);
	fixture_free(&fixture);
	warn_capture_end();

	CASE("field push: absent dependent ports load 0, which is the fallback sentinel");
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(""));
	CHECK_UINT(fixture.cfg->channel[0].sip_outbound_proxy_port, 0);
	CHECK_UINT(fixture.cfg->channel[0].sip_registrar_port, 0);
	fixture_free(&fixture);

	CASE("field push: empty dependent ports are dropped by libuci and "
	     "load 0");
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(
		"\toption sip_outbound_proxy_port ''\n"
		"\toption sip_registrar_port ''\n"));
	CHECK_UINT(fixture.cfg->channel[0].sip_outbound_proxy_port, 0);
	CHECK_UINT(fixture.cfg->channel[0].sip_registrar_port, 0);
	fixture_free(&fixture);

	CASE("field push: dependent ports of '0' load 0 and warn not at all");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(
		"\toption sip_outbound_proxy_port '0'\n"
		"\toption sip_registrar_port '0'\n"));
	CHECK_UINT(fixture.cfg->channel[0].sip_outbound_proxy_port, 0);
	CHECK_UINT(fixture.cfg->channel[0].sip_registrar_port, 0);
	CHECK_UINT(warn_capture_count("sip_outbound_proxy_port"), 0);
	CHECK_UINT(warn_capture_count("sip_registrar_port"), 0);
	fixture_free(&fixture);
	warn_capture_end();

	CASE("field push: the digit map base64 decodes to the pushed map");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(field_config));
	CHECK_CSTR(fixture.cfg->channel[0].digit_map,
		   "[2-9]11|[2-9]xxxxxxxxx|1[2-9]xxxxxxxxx|*55|*56|*57|*69|*72"
		   "|*73|*81|*90|*98|xx.T|[*#][*#x].T|xx.#");
	fixture_free(&fixture);

	CASE("field push: the decoded field map compiles and warns not at all");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(field_config));
	CHECK_UINT(warn_capture_count("is not a valid digit map"), 0);
	fixture_free(&fixture);
	warn_capture_end();

	/*
	 * The in-service predicate of channel 1 (present, enabled, and the
	 * proxy and username acct_cfg_derive() needs to call it registrable),
	 * and channel 2's own predicate the other way: present, disabled, and
	 * its username and secret loaded anyway. The push case above reads
	 * neither channel's username or secret, so an index or a section
	 * bookkeeping defect that leaves channel 2 unparsed on disabled (and
	 * so still reads channel 1's enabled flag as true) fails only here.
	 */
	CASE("field push: channel 2 disabled leaves channel 1 in service");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(field_config));
	CHECK(fixture.cfg->channel[0].present);
	CHECK(fixture.cfg->channel[0].enabled);
	CHECK(fixture.cfg->channel[0].sip_proxy[0] != '\0');
	CHECK(fixture.cfg->channel[0].sip_username[0] != '\0');
	CHECK(fixture.cfg->channel[1].present);
	CHECK(!fixture.cfg->channel[1].enabled);
	CHECK_CSTR(fixture.cfg->channel[1].sip_username, "15551234568");
	CHECK_CSTR(fixture.cfg->channel[1].sip_secret, "s3cr3t2");
	fixture_free(&fixture);

	CASE("field push: a second load that moves channel 2 leaves every channel 1 field equal");
	{
		struct cfg_fixture moved;

		FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(field_config));
		moved = fixture_load(field_config_moved);
		if (!fixture_ready(&moved)) {
			fixture_free(&fixture);
			return;
		}
		CHECK(!memcmp(&fixture.cfg->channel[0], &moved.cfg->channel[0],
			      sizeof(fixture.cfg->channel[0])));
		fixture_free(&fixture);
		fixture_free(&moved);
	}

	CASE("field push: local_interface 'wan' loads as written");
	FIXTURE_LOAD_OR_RETURN(fixture,
		fixture_load("config base 'base'\n\toption local_interface 'wan'\n"));
	CHECK_CSTR(fixture.cfg->local_interface, "wan");
	fixture_free(&fixture);

	CASE("field push: local_interface 'voice' loads as written");
	FIXTURE_LOAD_OR_RETURN(fixture,
		fixture_load("config base 'base'\n\toption local_interface 'voice'\n"));
	CHECK_CSTR(fixture.cfg->local_interface, "voice");
	fixture_free(&fixture);

	CASE("field push: an empty local_interface takes the schema default "
	     "wan");
	FIXTURE_LOAD_OR_RETURN(fixture,
		fixture_load("config base 'base'\n\toption local_interface ''\n"));
	CHECK_CSTR(fixture.cfg->local_interface, "wan");
	fixture_free(&fixture);

	CASE("field push: local_interface 'undefined' loads as written and warns not at all");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture,
		fixture_load("config base 'base'\n\toption local_interface 'undefined'\n"));
	CHECK_CSTR(fixture.cfg->local_interface, "undefined");
	CHECK_UINT(warn_capture_count("local_interface"), 0);
	fixture_free(&fixture);
	warn_capture_end();

	/* channels and vmwi (the option the fixture text names right before
	 * the two unknown options) prove the parser reached the end of the
	 * section and moved on to channel 2, not just that channel 1 itself
	 * still looked fine */
	CASE("field push: directory_number and tr104_sip_client_alias load the file and warn not at all");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(field_config));
	CHECK_UINT(fixture.cfg->channels, 2);
	CHECK(fixture.cfg->channel[0].vmwi);
	CHECK_UINT(warn_capture_count("directory_number"), 0);
	CHECK_UINT(warn_capture_count("tr104_sip_client_alias"), 0);
	fixture_free(&fixture);
	warn_capture_end();

	CASE("field push: cn_enable, amwi and vmwi load their pushed values");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(field_config));
	CHECK(!fixture.cfg->channel[0].cn_enable);
	CHECK(fixture.cfg->channel[0].amwi);
	CHECK(fixture.cfg->channel[0].vmwi);
	fixture_free(&fixture);

	CASE("field push: sip_dtmf_mode 'inband' loads inband");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption sip_dtmf_mode 'inband'\n"));
	CHECK(fixture.cfg->channel[0].dtmf_mode == CFG_DTMF_INBAND);
	fixture_free(&fixture);

	CASE("field push: sip_dtmf_mode 'rfc2833' loads rfc2833");
	FIXTURE_LOAD_OR_RETURN(fixture,
		channel_option_load("\toption sip_dtmf_mode 'rfc2833'\n"));
	CHECK(fixture.cfg->channel[0].dtmf_mode == CFG_DTMF_RFC2833);
	fixture_free(&fixture);

	CASE("field push: both channels enabled load in service");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(field_config_moved));
	CHECK(fixture.cfg->channel[0].enabled);
	CHECK(fixture.cfg->channel[1].enabled);
	fixture_free(&fixture);
}

/*
 * The support class of the five TLS options. Nothing in the daemon reads enum
 * cfg_support, so the descriptor table is the only place that holds the
 * answer. A row that read accepted-unsupported would tell an operator that
 * the daemon ignores an option that the loader carries.
 */
static void tls_support_test(void)
{
	static const char *const names[] = {
		"tls_cert_path", "tls_cert_username", "tls_cert_password",
		"tls_server_verification_enabled", "tls_keylog_enabled",
	};
	const struct cfg_option_desc *d;
	unsigned int found = 0;
	unsigned int i, j;

	CASE("schema: the five base TLS options are supported and restart all");
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		for (j = 0; cfg_base_options[j].name; j++) {
			d = &cfg_base_options[j];
			if (strcmp(d->name, names[i]))
				continue;

			found++;
			CHECK_UINT(d->support, SUP_SUPPORTED);
			CHECK_UINT(d->apply_class, APPLY_RESTART_ALL);
		}
	}
	CHECK_UINT(found, ARRAY_SIZE(names));
}

/*
 * The support class of the two transport rows. The channel row is the one
 * channel option that is not normalized to its base twin. The account reads
 * transport from the base section alone (account.h). A channel value is
 * therefore accepted, reported once and ignored. The descriptor table is the
 * only place where an operator can read that, and the base row stays the
 * supported one.
 */
static void transport_support_test(void)
{
	const struct cfg_option_desc *d;
	unsigned int found = 0;
	unsigned int i;

	CASE("schema: the channel transport row is accepted and unsupported");
	for (i = 0; cfg_channel_options[i].name; i++) {
		d = &cfg_channel_options[i];
		if (strcmp(d->name, "transport"))
			continue;

		found++;
		CHECK_UINT(d->support, SUP_ACCEPTED_UNSUPPORTED);
		CHECK_UINT(d->apply_class, APPLY_RESTART_ALL);
		/* and it names no alias target: nothing normalizes the row
		 * into the base one, the account simply reads the base */
		CHECK(d->alias_of == NULL);
	}
	CHECK_UINT(found, 1);

	CASE("schema: the base transport row stays supported and restarts all");
	found = 0;
	for (i = 0; cfg_base_options[i].name; i++) {
		d = &cfg_base_options[i];
		if (strcmp(d->name, "transport"))
			continue;

		found++;
		CHECK_UINT(d->support, SUP_SUPPORTED);
		CHECK_UINT(d->apply_class, APPLY_RESTART_ALL);
	}
	CHECK_UINT(found, 1);

	/* the option rides the registration, so a move rebuilds it and not the
	 * transport under it, which is the class of its sibling */
	CASE("schema: the dns_move_unregister row is supported and "
	     "re-registers");
	found = 0;
	for (i = 0; cfg_base_options[i].name; i++) {
		d = &cfg_base_options[i];
		if (strcmp(d->name, "dns_move_unregister"))
			continue;

		found++;
		CHECK_UINT(d->type, CFG_BOOL);
		CHECK_CSTR(d->def, "1");
		CHECK_UINT(d->support, SUP_SUPPORTED);
		CHECK_UINT(d->apply_class, APPLY_REREG);
	}
	CHECK_UINT(found, 1);

	/* the row and the hand-written default in cfg_defaults() are two
	 * records of one value, and this case is where they meet */
	CASE("schema: the dns_srv_mode row is supported and re-registers");
	found = 0;
	for (i = 0; cfg_base_options[i].name; i++) {
		d = &cfg_base_options[i];
		if (strcmp(d->name, "dns_srv_mode"))
			continue;

		found++;
		CHECK_UINT(d->type, CFG_ENUM);
		CHECK_CSTR(d->def, "auto");
		CHECK(d->enum_vals == cfg_dns_srv_mode_vals);
		CHECK_UINT(d->support, SUP_SUPPORTED);
		CHECK_UINT(d->apply_class, APPLY_REREG);
	}
	CHECK_UINT(found, 1);
}

/*
 * The options that no consumer reads. The loader loads, bounds-checks and
 * reports each one, and nothing in src/ names it. The support class is
 * therefore the only place where an operator learns that the daemon ignores
 * it. The cases find each named option in the descriptor tables of
 * src/cfg/schema.h and check that its class is SUP_ACCEPTED_UNSUPPORTED.
 */
static void unread_support_test(void)
{
	static const char *const base_names[] = {
		"preset_log_level", "active_source_filter_enabled",
	};
	static const char *const codec_names[] = {
		"tr104_name", "cwmp_alias",
	};
	static const char *const chan_names[] = {
		"voicemail_exp_time",
		"speed_dial_num_0", "speed_dial_num_1", "speed_dial_num_2",
		"speed_dial_num_3", "speed_dial_num_4", "speed_dial_num_5",
		"speed_dial_num_6", "speed_dial_num_7", "speed_dial_num_8",
		"speed_dial_num_9",
		"block_number_enable",
		"block_number_0", "block_number_1", "block_number_2",
		"block_number_3", "block_number_4", "block_number_5",
		"block_number_6", "block_number_7", "block_number_8",
		"block_number_9",
		"allow_number_enable",
		"allow_number_0", "allow_number_1", "allow_number_2",
		"allow_number_3", "allow_number_4", "allow_number_5",
		"allow_number_6", "allow_number_7", "allow_number_8",
		"allow_number_9",
	};
	const struct cfg_option_desc *d;
	unsigned int found = 0;
	unsigned int i, j;

	CASE("schema: the two unread base options are accepted and unsupported");
	for (i = 0; i < ARRAY_SIZE(base_names); i++) {
		for (j = 0; cfg_base_options[j].name; j++) {
			d = &cfg_base_options[j];
			if (strcmp(d->name, base_names[i]))
				continue;

			found++;
			CHECK_UINT(d->support, SUP_ACCEPTED_UNSUPPORTED);
		}
	}
	CHECK_UINT(found, ARRAY_SIZE(base_names));

	CASE("schema: the speed dial, block and allow rows are accepted and unsupported");
	found = 0;
	for (i = 0; i < ARRAY_SIZE(chan_names); i++) {
		for (j = 0; cfg_channel_options[j].name; j++) {
			d = &cfg_channel_options[j];
			if (strcmp(d->name, chan_names[i]))
				continue;

			found++;
			CHECK_UINT(d->support, SUP_ACCEPTED_UNSUPPORTED);
		}
	}
	CHECK_UINT(found, ARRAY_SIZE(chan_names));

	CASE("schema: the two unread codec rows are accepted and unsupported");
	found = 0;
	for (i = 0; i < ARRAY_SIZE(codec_names); i++) {
		for (j = 0; cfg_codec_options[j].name; j++) {
			d = &cfg_codec_options[j];
			if (strcmp(d->name, codec_names[i]))
				continue;

			found++;
			CHECK_UINT(d->support, SUP_ACCEPTED_UNSUPPORTED);
		}
	}
	CHECK_UINT(found, ARRAY_SIZE(codec_names));

	/* the loader parses this row by name and the config dump reports it,
	 * so src/ names it; the call control reads it nowhere */
	CASE("schema: the flash service timer row is accepted and unsupported");
	found = 0;
	for (j = 0; cfg_channel_options[j].name; j++) {
		d = &cfg_channel_options[j];
		if (strcmp(d->name, "flash_service_timer_sec"))
			continue;

		found++;
		CHECK_UINT(d->support, SUP_ACCEPTED_UNSUPPORTED);
	}
	CHECK_UINT(found, 1);
}

/*
 * The two transport rows read through the enum loader. Both are CFG_ENUM, and
 * the vocabulary is closed: account.c maps udp, tcp and tls and nothing else.
 * An unknown token therefore takes the answer of the loader and never reaches
 * the account as text. The base row takes its schema default "udp". The
 * channel row has no default and stays empty, which is the value that
 * inherits the base transport. acct_transport_derive() keeps its own
 * unknown-token branch as a second defense, so this suite reads the loaded
 * strings and not the derive.
 */
static void transport_enum_tests(void)
{
	enum {
		DUMP_TRANSPORT,
		DUMP_MAX,
	};
	static const struct blobmsg_policy dump_policy[DUMP_MAX] = {
		[DUMP_TRANSPORT] = {
			.name = "transport",
			.type = BLOBMSG_TYPE_STRING,
		},
	};
	struct blob_attr *dump_fields[DUMP_MAX] = { 0 };
	struct cfg_fixture fixture;
	struct blob_buf buf = { 0 };

	/*
	 * The warning names the section and the outcome. Both transport rows
	 * carry the same option name, so an unattributed line cannot say which
	 * section faulted, and the channel row names no default, so its
	 * rejected value is ignored rather than replaced.
	 */
	CASE("an unknown base transport warns once and takes the schema default");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n"
		"\toption channels '1'\n"
		"\toption transport 'sctp'\n"));
	CHECK_CSTR(fixture.cfg->transport, "udp");
	CHECK_UINT(warn_capture_count(
		"voip base transport value 'sctp' is invalid, using default 'udp'"), 1);
	fixture_free(&fixture);

	CASE("a known base transport loads unchanged and warns not at all");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n"
		"\toption channels '1'\n"
		"\toption transport 'tls'\n"));
	CHECK_CSTR(fixture.cfg->transport, "tls");
	CHECK_UINT(warn_capture_count("transport value"), 0);
	fixture_free(&fixture);

	CASE("an unknown channel transport warns once and reads as inherited");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(
		"\toption transport 'quic'\n"));
	CHECK_CSTR(fixture.cfg->channel[0].transport, "");
	CHECK_UINT(warn_capture_count(
		"voip channel1 transport value 'quic' is invalid, ignored"), 1);

	CASE("and the channel fault names no default, because the row has none");
	CHECK_UINT(warn_capture_count("using default"), 0);
	fixture_free(&fixture);

	CASE("a known channel transport loads unchanged and warns not at all");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(
		"\toption transport 'tcp'\n"));
	CHECK_CSTR(fixture.cfg->channel[0].transport, "tcp");
	CHECK_UINT(warn_capture_count("transport value"), 0);
	fixture_free(&fixture);

	CASE("an absent channel transport stays empty and warns not at all");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(""));
	CHECK_CSTR(fixture.cfg->channel[0].transport, "");
	CHECK_UINT(warn_capture_count("transport value"), 0);
	fixture_free(&fixture);
	warn_capture_end();

	/*
	 * The dump reports what the daemon runs. A faulted config therefore
	 * reads one way through `uci get voip.base.transport`, which answers
	 * with the text the file carries, and another way here.
	 */
	CASE("the dump reports the normalized token of a faulted base transport");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n"
		"\toption channels '1'\n"
		"\toption transport 'sctp'\n"));
	blobmsg_buf_init(&buf);
	CHECK(buf.head != NULL);
	if (!buf.head) {
		blob_buf_free(&buf);
		fixture_free(&fixture);
		return;
	}

	cfg_dump(fixture.cfg, &buf);
	blobmsg_parse_attr(dump_policy, DUMP_MAX, dump_fields, buf.head);
	CHECK(dump_fields[DUMP_TRANSPORT] != NULL);
	if (dump_fields[DUMP_TRANSPORT])
		CHECK_CSTR(blobmsg_get_string(dump_fields[DUMP_TRANSPORT]), "udp");

	blob_buf_free(&buf);
	fixture_free(&fixture);
}

/*
 * voip.base.protocol, the signaling protocol of the daemon. The row is a
 * CFG_ENUM on the transport idiom: the two words load as written, an unknown
 * word warns once and takes the default, and cfg_protocol_of() maps the word.
 * A move needs a daemon restart, which the apply class says.
 */
static void protocol_tests(void)
{
	enum {
		DUMP_PROTOCOL,
		DUMP_MAX,
	};
	static const struct blobmsg_policy dump_policy[DUMP_MAX] = {
		[DUMP_PROTOCOL] = {
			.name = "protocol",
			.type = BLOBMSG_TYPE_STRING,
		},
	};
	struct blob_attr *dump_fields[DUMP_MAX] = { 0 };
	const struct cfg_option_desc *d;
	struct cfg_fixture fixture;
	struct blob_buf buf = { 0 };
	unsigned int found = 0;
	unsigned int i;

	CASE("protocol: the schema row is a supported enum that restarts the "
	     "daemon");
	for (i = 0; cfg_base_options[i].name; i++) {
		d = &cfg_base_options[i];
		if (strcmp(d->name, "protocol"))
			continue;

		found++;
		CHECK_UINT(d->type, CFG_ENUM);
		CHECK_CSTR(d->def, "sip");
		CHECK_UINT(d->support, SUP_SUPPORTED);
		CHECK_UINT(d->apply_class, APPLY_RESTART_DAEMON);
		CHECK(d->enum_vals && d->enum_vals[0] &&
		      !strcmp(d->enum_vals[0], "sip"));
		CHECK(d->enum_vals && d->enum_vals[1] &&
		      !strcmp(d->enum_vals[1], "mgcp"));
		CHECK(d->enum_vals && !d->enum_vals[2]);
	}
	CHECK_UINT(found, 1);

	CASE("protocol: the restart class follows every class a reload "
	     "applies");
	CHECK(APPLY_RESTART_DAEMON > APPLY_RESTART_ALL);

	CASE("protocol: an absent option loads sip and maps to SIP");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n"
		"\toption channels '1'\n"));
	CHECK_CSTR(fixture.cfg->protocol, "sip");
	CHECK_UINT(cfg_protocol_of(fixture.cfg), CFG_PROTO_SIP);
	CHECK_UINT(warn_capture_count("protocol value"), 0);
	fixture_free(&fixture);

	CASE("protocol: the mgcp word loads unchanged and maps to MGCP");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n"
		"\toption channels '1'\n"
		"\toption protocol 'mgcp'\n"));
	CHECK_CSTR(fixture.cfg->protocol, "mgcp");
	CHECK_UINT(cfg_protocol_of(fixture.cfg), CFG_PROTO_MGCP);
	CHECK_UINT(warn_capture_count("protocol value"), 0);
	fixture_free(&fixture);

	CASE("protocol: an unknown word warns once and takes the sip default");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n"
		"\toption channels '1'\n"
		"\toption protocol 'h323'\n"));
	CHECK_CSTR(fixture.cfg->protocol, "sip");
	CHECK_UINT(cfg_protocol_of(fixture.cfg), CFG_PROTO_SIP);
	CHECK_UINT(warn_capture_count(
		"voip base protocol value 'h323' is invalid, using default 'sip'"), 1);
	fixture_free(&fixture);
	warn_capture_end();

	CASE("protocol: an empty word maps to SIP");
	{
		struct cfg empty;

		memset(&empty, 0, sizeof(empty));
		CHECK_UINT(cfg_protocol_of(&empty), CFG_PROTO_SIP);
	}

	CASE("protocol: the dump reports the word the daemon runs");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n"
		"\toption channels '1'\n"
		"\toption protocol 'mgcp'\n"));
	blobmsg_buf_init(&buf);
	CHECK(buf.head != NULL);
	if (!buf.head) {
		blob_buf_free(&buf);
		fixture_free(&fixture);
		return;
	}

	cfg_dump(fixture.cfg, &buf);
	blobmsg_parse_attr(dump_policy, DUMP_MAX, dump_fields, buf.head);
	CHECK(dump_fields[DUMP_PROTOCOL] != NULL);
	if (dump_fields[DUMP_PROTOCOL])
		CHECK_CSTR(blobmsg_get_string(dump_fields[DUMP_PROTOCOL]),
			   "mgcp");

	blob_buf_free(&buf);
	fixture_free(&fixture);
}

static struct cfg_fixture mgcp_option_load(const char *option)
{
	char config[2048];

	snprintf(config, sizeof(config), "config mgcp 'mgcp'\n%s", option);

	return fixture_load(config);
}

static const char mgcp_full_config[] =
	"config base 'base'\n"
	"\toption channels '1'\n"
	"\toption protocol 'sip'\n"
	"config mgcp 'mgcp'\n"
	"\toption call_agent 'ca.example.test'\n"
	"\toption call_agent_port '2728'\n"
	"\toption local_port '2429'\n"
	"\toption domain 'gw.example.test'\n"
	"\toption bracketed_ip '1'\n"
	"\toption endpoint_pattern 'ep/{line}@{host}'\n"
	"\toption line_base '0'\n"
	"\toption dialect 'ncs'\n"
	"\toption max1 '4'\n"
	"\toption max2 '9'\n"
	"\toption rto_init_ms '250'\n"
	"\toption rto_max_ms '8000'\n"
	"\toption t_max_s '25'\n"
	"\toption t_hist_s '40'\n"
	"\toption persistent_events 'hd,L/hu'\n"
	"\toption quarantine_default 'loop'\n"
	"\toption send_ptime '0'\n"
	"\toption reply_method 'mgc_port'\n"
	"\toption signaling_dscp '46'\n"
	"\toption restart_delay_max_s '600'\n";

/*
 * The mgcp section. Every row is supported and declares the class
 * APPLY_RESTART_ALL.
 */
static void mgcp_schema_test(void)
{
	const struct cfg_option_desc *d;
	unsigned int count = 0;

	CASE("schema: every mgcp option is supported and restarts all");
	for (d = cfg_mgcp_options; d->name; d++) {
		CHECK_UINT(d->support, SUP_SUPPORTED);
		CHECK_UINT(d->apply_class, APPLY_RESTART_ALL);
		CHECK(d->alias_of == NULL);
		count++;
	}
	CHECK_UINT(count, 20);
}

/*
 * An absent section, and an absent package, load every default. The
 * transaction defaults are the RFC 3435 values of the MGCP transaction
 * layer.
 */
static void mgcp_default_tests(void)
{
	struct cfg_fixture fixture;
	const struct cfg_mgcp *m;

	CASE("mgcp: an absent section loads the call agent defaults");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n\toption channels '1'\n"));
	m = &fixture.cfg->mgcp;
	CHECK_CSTR(m->call_agent, "");
	CHECK_UINT(m->call_agent_port, 2727);
	CHECK_UINT(m->local_port, 2427);

	CASE("mgcp: an absent section loads the endpoint name defaults");
	CHECK_CSTR(m->domain, "");
	CHECK(!m->bracketed_ip);
	CHECK_CSTR(m->endpoint_pattern, "aaln/{line}@{host}");
	CHECK_UINT(m->line_base, 1);

	CASE("mgcp: an absent section loads the rfc3435 dialect");
	CHECK_CSTR(m->dialect, "rfc3435");
	CHECK_UINT(cfg_mgcp_dialect_of(fixture.cfg), CFG_MGCP_RFC3435);

	CASE("mgcp: an absent section loads the transaction defaults");
	CHECK_UINT(m->max1, 5);
	CHECK_UINT(m->max2, 7);
	CHECK_UINT(m->rto_init_ms, 200);
	CHECK_UINT(m->rto_max_ms, 4000);
	CHECK_UINT(m->t_max_s, 20);
	CHECK_UINT(m->t_hist_s, 30);

	CASE("mgcp: an absent section loads the event and media defaults");
	CHECK_CSTR(m->persistent_events, "hd,hu,hf");
	CHECK(m->quarantine_default == CFG_MGCP_QUARANTINE_STEP);
	CHECK(m->send_ptime);
	CHECK(m->reply_method == CFG_MGCP_REPLY_SOURCE);

	CASE("mgcp: an absent section loads the socket and restart defaults");
	CHECK_UINT(m->signaling_dscp, 24);
	CHECK_UINT(m->restart_delay_max_s, 10);

	CASE("mgcp: an absent section warns not at all");
	CHECK_UINT(warn_capture_count("mgcp"), 0);
	CHECK_UINT(warn_capture_count("clamped"), 0);
	warn_capture_end();
	fixture_free(&fixture);

	CASE("mgcp: a missing voip package loads the same defaults");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(NULL));
	m = &fixture.cfg->mgcp;
	CHECK(!fixture.cfg->loaded);
	CHECK_UINT(m->call_agent_port, 2727);
	CHECK_CSTR(m->endpoint_pattern, "aaln/{line}@{host}");
	CHECK_CSTR(m->dialect, "rfc3435");
	CHECK_UINT(m->max2, 7);
	CHECK_UINT(m->t_hist_s, 30);
	CHECK_CSTR(m->persistent_events, "hd,hu,hf");
	CHECK(m->send_ptime);
	CHECK_UINT(m->restart_delay_max_s, 10);
	fixture_free(&fixture);
}

/*
 * Every option loads from the section, under the protocol word sip too: the
 * loader is core and reads the section whatever protocol the daemon runs.
 */
static void mgcp_load_tests(void)
{
	struct cfg_fixture fixture;
	const struct cfg_mgcp *m;

	CASE("mgcp: the section loads under protocol sip");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(mgcp_full_config));
	CHECK_UINT(cfg_protocol_of(fixture.cfg), CFG_PROTO_SIP);
	m = &fixture.cfg->mgcp;
	CHECK_CSTR(m->call_agent, "ca.example.test");

	CASE("mgcp: the call agent options load");
	CHECK_UINT(m->call_agent_port, 2728);
	CHECK_UINT(m->local_port, 2429);

	CASE("mgcp: the endpoint name options load");
	CHECK_CSTR(m->domain, "gw.example.test");
	CHECK(m->bracketed_ip);
	CHECK_CSTR(m->endpoint_pattern, "ep/{line}@{host}");
	CHECK_UINT(m->line_base, 0);

	CASE("mgcp: the ncs dialect loads and maps to NCS");
	CHECK_CSTR(m->dialect, "ncs");
	CHECK_UINT(cfg_mgcp_dialect_of(fixture.cfg), CFG_MGCP_NCS);

	CASE("mgcp: the transaction options load");
	CHECK_UINT(m->max1, 4);
	CHECK_UINT(m->max2, 9);
	CHECK_UINT(m->rto_init_ms, 250);
	CHECK_UINT(m->rto_max_ms, 8000);
	CHECK_UINT(m->t_max_s, 25);
	CHECK_UINT(m->t_hist_s, 40);

	CASE("mgcp: the event and media options load");
	CHECK_CSTR(m->persistent_events, "hd,L/hu");
	CHECK(m->quarantine_default == CFG_MGCP_QUARANTINE_LOOP);
	CHECK(!m->send_ptime);
	CHECK(m->reply_method == CFG_MGCP_REPLY_MGC_PORT);

	CASE("mgcp: the socket and restart options load");
	CHECK_UINT(m->signaling_dscp, 46);
	CHECK_UINT(m->restart_delay_max_s, 600);

	CASE("mgcp: a full section warns not at all");
	CHECK_UINT(warn_capture_count("mgcp"), 0);
	CHECK_UINT(warn_capture_count("clamped"), 0);
	warn_capture_end();
	fixture_free(&fixture);

	CASE("mgcp: the section loads under protocol mgcp");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n\toption protocol 'mgcp'\n"
		"config mgcp 'mgcp'\n\toption call_agent '192.0.2.10'\n"));
	CHECK_UINT(cfg_protocol_of(fixture.cfg), CFG_PROTO_MGCP);
	CHECK_CSTR(fixture.cfg->mgcp.call_agent, "192.0.2.10");
	fixture_free(&fixture);

	CASE("mgcp: an anonymous section loads as the first mgcp section");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config mgcp\n\toption local_port '2430'\n"));
	CHECK_UINT(fixture.cfg->mgcp.local_port, 2430);
	fixture_free(&fixture);

	CASE("mgcp: the first section wins and each additional section warns");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config mgcp 'first'\n\toption local_port '2431'\n"
		"config mgcp 'mgcp'\n\toption local_port '2432'\n"
		"config mgcp\n\toption local_port '2433'\n"));
	CHECK_UINT(fixture.cfg->mgcp.local_port, 2431);
	CHECK_UINT(warn_capture_count("additional mgcp section ignored"), 2);
	warn_capture_end();
	fixture_free(&fixture);

	/* every other section carries values off its defaults, so a load of
	 * the mgcp section that reset one of them shows here */
	CASE("mgcp: the other sections keep their values beside the section");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n\toption channels '1'\n"
		"\toption protocol 'mgcp'\n"
		"config hal 'hal'\n\toption tdm_backend 'file'\n"
		"\toption prefill_periods '9'\n"
		"config debug 'debug'\n\toption trace 'media,jb'\n"
		"config mgcp 'mgcp'\n\toption local_port '2429'\n"));
	CHECK_UINT(fixture.cfg->channels, 1);
	CHECK_CSTR(fixture.cfg->protocol, "mgcp");
	CHECK_CSTR(fixture.cfg->hal.tdm_backend, "file");
	CHECK_UINT(fixture.cfg->hal.prefill_periods, 9);
	CHECK_CSTR(fixture.cfg->debug.trace, "media,jb");
	CHECK_UINT(fixture.cfg->mgcp.local_port, 2429);
	fixture_free(&fixture);
}

/*
 * The three enum rows on the transport idiom: an unknown word warns once,
 * names the section, and takes the default.
 */
static void mgcp_enum_tests(void)
{
	struct cfg_fixture fixture;

	CASE("mgcp: an unknown dialect warns once and takes rfc3435");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture,
		mgcp_option_load("\toption dialect 'ncs2'\n"));
	CHECK_CSTR(fixture.cfg->mgcp.dialect, "rfc3435");
	CHECK_UINT(cfg_mgcp_dialect_of(fixture.cfg), CFG_MGCP_RFC3435);
	CHECK_UINT(warn_capture_count(
		"voip mgcp dialect value 'ncs2' is invalid, using default "
		"'rfc3435'"), 1);
	warn_capture_end();
	fixture_free(&fixture);

	CASE("mgcp: the rfc3435 dialect loads and warns not at all");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture,
		mgcp_option_load("\toption dialect 'rfc3435'\n"));
	CHECK_CSTR(fixture.cfg->mgcp.dialect, "rfc3435");
	CHECK_UINT(warn_capture_count("dialect value"), 0);
	warn_capture_end();
	fixture_free(&fixture);

	CASE("mgcp: an empty dialect word maps to RFC3435");
	{
		struct cfg empty;

		memset(&empty, 0, sizeof(empty));
		CHECK_UINT(cfg_mgcp_dialect_of(&empty), CFG_MGCP_RFC3435);
	}

	CASE("mgcp: an unknown quarantine_default warns once and takes step");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture,
		mgcp_option_load("\toption quarantine_default 'drop'\n"));
	CHECK(fixture.cfg->mgcp.quarantine_default ==
	      CFG_MGCP_QUARANTINE_STEP);
	CHECK_UINT(warn_capture_count(
		"voip mgcp quarantine_default value 'drop' is invalid, using "
		"default 'step'"), 1);
	warn_capture_end();
	fixture_free(&fixture);

	CASE("mgcp: an unknown reply_method warns once and takes source");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture,
		mgcp_option_load("\toption reply_method 'peer'\n"));
	CHECK(fixture.cfg->mgcp.reply_method == CFG_MGCP_REPLY_SOURCE);
	CHECK_UINT(warn_capture_count(
		"voip mgcp reply_method value 'peer' is invalid, using default "
		"'source'"), 1);
	warn_capture_end();
	fixture_free(&fixture);

	CASE("mgcp: a malformed bool warns once and takes its default");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture,
		mgcp_option_load("\toption send_ptime 'maybe'\n"));
	CHECK(fixture.cfg->mgcp.send_ptime);
	CHECK_UINT(warn_capture_count(
		"voip send_ptime value 'maybe' is not a boolean"), 1);
	warn_capture_end();
	fixture_free(&fixture);
}

/*
 * One numeric load of the mgcp section and the warnings it costs. The case
 * counts every clamp, parse and rule warning of the load, so a value that
 * warns twice fails even when the named warning is among them.
 */
struct mgcp_bound {
	const char *options;	/* the option lines of the section */
	size_t field;		/* offsetof() an unsigned int of cfg_mgcp */
	unsigned int want;
	const char *warn;	/* a warning of the load, or NULL */
	unsigned int warns;	/* the count of all of them */
};

static void mgcp_bound_case(const char *name, const struct mgcp_bound *b)
{
	struct cfg_fixture fixture;
	const char *base;

	CASE(name);
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, mgcp_option_load(b->options));
	base = (const char *)&fixture.cfg->mgcp;
	CHECK_UINT(*(const unsigned int *)(base + b->field), b->want);
	if (b->warn)
		CHECK_UINT(warn_capture_count(b->warn), 1);
	CHECK_UINT(warn_capture_count("clamped") +
		   warn_capture_count("is not a number") +
		   warn_capture_count("is invalid"), b->warns);
	warn_capture_end();
	fixture_free(&fixture);
}

#define MGCP_BOUND_N(opts, member, value, needle, count) \
	(&(const struct mgcp_bound){ (opts), \
		offsetof(struct cfg_mgcp, member), (value), (needle), (count) })
#define MGCP_BOUND(opts, member, value, needle) \
	MGCP_BOUND_N(opts, member, value, needle, (needle) ? 1 : 0)

/*
 * The numeric rows clamp to their bounds and warn once, the uint_clamp()
 * idiom of the loader. A row whose low bound is 0 cannot go below it: a
 * sign does not parse, so the value warns and takes the default.
 */
static void mgcp_range_tests(void)
{
	mgcp_bound_case("mgcp range: call_agent_port 1 is the low bound",
		MGCP_BOUND("\toption call_agent_port '1'\n",
			   call_agent_port, 1, NULL));
	mgcp_bound_case("mgcp range: call_agent_port 0 clamps to 1",
		MGCP_BOUND("\toption call_agent_port '0'\n",
			   call_agent_port, 1,
			   "voip call_agent_port 0 below 1, clamped"));
	mgcp_bound_case("mgcp range: call_agent_port 65535 is the high bound",
		MGCP_BOUND("\toption call_agent_port '65535'\n",
			   call_agent_port, 65535, NULL));
	mgcp_bound_case("mgcp range: call_agent_port 65536 clamps to 65535",
		MGCP_BOUND("\toption call_agent_port '65536'\n",
			   call_agent_port, 65535,
			   "voip call_agent_port 65536 above 65535, clamped"));
	mgcp_bound_case("mgcp range: local_port 1 is the low bound",
		MGCP_BOUND("\toption local_port '1'\n", local_port, 1, NULL));
	mgcp_bound_case("mgcp range: local_port 0 clamps to 1",
		MGCP_BOUND("\toption local_port '0'\n", local_port, 1,
			   "voip local_port 0 below 1, clamped"));
	mgcp_bound_case("mgcp range: local_port 65535 is the high bound",
		MGCP_BOUND("\toption local_port '65535'\n", local_port, 65535,
			   NULL));
	mgcp_bound_case("mgcp range: local_port 70000 clamps to 65535",
		MGCP_BOUND("\toption local_port '70000'\n", local_port, 65535,
			   "voip local_port 70000 above 65535, clamped"));
	mgcp_bound_case("mgcp range: line_base 0 is the low bound",
		MGCP_BOUND("\toption line_base '0'\n", line_base, 0, NULL));
	mgcp_bound_case("mgcp range: a negative line_base takes the default",
		MGCP_BOUND("\toption line_base '-1'\n", line_base, 1,
			   "voip line_base value '-1' is not a number"));
	mgcp_bound_case("mgcp range: line_base 255 is the high bound",
		MGCP_BOUND("\toption line_base '255'\n", line_base, 255,
			   NULL));
	mgcp_bound_case("mgcp range: line_base 256 clamps to 255",
		MGCP_BOUND("\toption line_base '256'\n", line_base, 255,
			   "voip line_base 256 above 255, clamped"));
	mgcp_bound_case("mgcp range: max1 1 is the low bound",
		MGCP_BOUND("\toption max1 '1'\n", max1, 1, NULL));
	mgcp_bound_case("mgcp range: max1 0 clamps to 1",
		MGCP_BOUND("\toption max1 '0'\n", max1, 1,
			   "voip max1 0 below 1, clamped"));
	mgcp_bound_case("mgcp range: max1 20 is the high bound",
		MGCP_BOUND("\toption max1 '20'\n\toption max2 '30'\n", max1,
			   20, NULL));
	mgcp_bound_case("mgcp range: max1 21 clamps to 20",
		MGCP_BOUND("\toption max1 '21'\n\toption max2 '30'\n", max1,
			   20, "voip max1 21 above 20, clamped"));
	mgcp_bound_case("mgcp range: max2 one above max1 is the low bound",
		MGCP_BOUND("\toption max2 '6'\n", max2, 6, NULL));
	mgcp_bound_case("mgcp range: max2 equal to max1 clamps to max1 + 1",
		MGCP_BOUND("\toption max2 '5'\n", max2, 6,
			   "voip max2 5 below max1 + 1 (6), clamped"));
	mgcp_bound_case("mgcp range: max2 2 is the low bound under max1 1",
		MGCP_BOUND("\toption max1 '1'\n\toption max2 '2'\n", max2, 2,
			   NULL));
	mgcp_bound_case("mgcp range: max2 1 clamps to 2",
		MGCP_BOUND("\toption max1 '1'\n\toption max2 '1'\n", max2, 2,
			   "voip max2 1 below 2, clamped"));
	mgcp_bound_case("mgcp range: max2 30 is the high bound",
		MGCP_BOUND("\toption max2 '30'\n", max2, 30, NULL));
	mgcp_bound_case("mgcp range: max2 31 clamps to 30",
		MGCP_BOUND("\toption max2 '31'\n", max2, 30,
			   "voip max2 31 above 30, clamped"));
	mgcp_bound_case("mgcp range: rto_init_ms 100 is the low bound",
		MGCP_BOUND("\toption rto_init_ms '100'\n", rto_init_ms, 100,
			   NULL));
	mgcp_bound_case("mgcp range: rto_init_ms 99 clamps to 100",
		MGCP_BOUND("\toption rto_init_ms '99'\n", rto_init_ms, 100,
			   "voip rto_init_ms 99 below 100, clamped"));
	mgcp_bound_case("mgcp range: rto_max_ms 99 clamps to rto_init_ms in "
			"one step",
		MGCP_BOUND("\toption rto_max_ms '99'\n", rto_max_ms, 200,
			   "voip rto_max_ms 99 below rto_init_ms (200), "
			   "clamped"));
	mgcp_bound_case("mgcp range: max2 0 clamps to max1 + 1 in one step",
		MGCP_BOUND("\toption max2 '0'\n", max2, 6,
			   "voip max2 0 below max1 + 1 (6), clamped"));
	mgcp_bound_case("mgcp range: t_hist_s 0 clamps to t_max_s in one step",
		MGCP_BOUND("\toption t_hist_s '0'\n", t_hist_s, 20,
			   "voip t_hist_s 0 below t_max_s (20), clamped"));
	mgcp_bound_case("mgcp range: a malformed max2 under a high max1 warns "
			"once",
		MGCP_BOUND("\toption max1 '10'\n\toption max2 'many'\n", max2,
			   11, "voip max2 value 'many' is not a number"));
	mgcp_bound_case("mgcp range: rto_init_ms 10000 is the high bound",
		MGCP_BOUND("\toption rto_init_ms '10000'\n"
			   "\toption rto_max_ms '60000'\n", rto_init_ms, 10000,
			   NULL));
	mgcp_bound_case("mgcp range: rto_init_ms 10001 clamps to 10000",
		MGCP_BOUND("\toption rto_init_ms '10001'\n"
			   "\toption rto_max_ms '60000'\n", rto_init_ms, 10000,
			   "voip rto_init_ms 10001 above 10000, clamped"));
	mgcp_bound_case("mgcp range: rto_max_ms equal to rto_init_ms is the "
			"low bound",
		MGCP_BOUND("\toption rto_max_ms '200'\n", rto_max_ms, 200,
			   NULL));
	mgcp_bound_case("mgcp range: rto_max_ms 60000 is the high bound",
		MGCP_BOUND("\toption rto_max_ms '60000'\n", rto_max_ms, 60000,
			   NULL));
	mgcp_bound_case("mgcp range: rto_max_ms 60001 clamps to 60000",
		MGCP_BOUND("\toption rto_max_ms '60001'\n", rto_max_ms, 60000,
			   "voip rto_max_ms 60001 above 60000, clamped"));
	mgcp_bound_case("mgcp range: t_max_s 1 is the low bound",
		MGCP_BOUND("\toption t_max_s '1'\n", t_max_s, 1, NULL));
	mgcp_bound_case("mgcp range: t_max_s 0 clamps to 1",
		MGCP_BOUND("\toption t_max_s '0'\n", t_max_s, 1,
			   "voip t_max_s 0 below 1, clamped"));
	mgcp_bound_case("mgcp range: t_max_s 300 is the high bound",
		MGCP_BOUND("\toption t_max_s '300'\n\toption t_hist_s '3600'\n",
			   t_max_s, 300, NULL));
	mgcp_bound_case("mgcp range: t_max_s 301 clamps to 300",
		MGCP_BOUND("\toption t_max_s '301'\n\toption t_hist_s '3600'\n",
			   t_max_s, 300,
			   "voip t_max_s 301 above 300, clamped"));
	mgcp_bound_case("mgcp range: t_hist_s equal to t_max_s is the low "
			"bound",
		MGCP_BOUND("\toption t_hist_s '20'\n", t_hist_s, 20, NULL));
	mgcp_bound_case("mgcp range: t_hist_s 3600 is the high bound",
		MGCP_BOUND("\toption t_hist_s '3600'\n", t_hist_s, 3600, NULL));
	mgcp_bound_case("mgcp range: t_hist_s 3601 clamps to 3600",
		MGCP_BOUND("\toption t_hist_s '3601'\n", t_hist_s, 3600,
			   "voip t_hist_s 3601 above 3600, clamped"));
	mgcp_bound_case("mgcp range: signaling_dscp 0 is the low bound",
		MGCP_BOUND("\toption signaling_dscp '0'\n", signaling_dscp, 0,
			   NULL));
	mgcp_bound_case("mgcp range: a negative signaling_dscp takes the "
			"default",
		MGCP_BOUND("\toption signaling_dscp '-1'\n", signaling_dscp,
			   24,
			   "voip signaling_dscp value '-1' is not a number"));
	mgcp_bound_case("mgcp range: signaling_dscp 63 is the high bound",
		MGCP_BOUND("\toption signaling_dscp '63'\n", signaling_dscp,
			   63, NULL));
	mgcp_bound_case("mgcp range: signaling_dscp 64 clamps to 63",
		MGCP_BOUND("\toption signaling_dscp '64'\n", signaling_dscp,
			   63, "voip signaling_dscp 64 above 63, clamped"));
	mgcp_bound_case("mgcp range: restart_delay_max_s 0 is the low bound",
		MGCP_BOUND("\toption restart_delay_max_s '0'\n",
			   restart_delay_max_s, 0, NULL));
	mgcp_bound_case("mgcp range: a negative restart_delay_max_s takes the "
			"default",
		MGCP_BOUND("\toption restart_delay_max_s '-1'\n",
			   restart_delay_max_s, 10,
			   "voip restart_delay_max_s value '-1' is not a "
			   "number"));
	mgcp_bound_case("mgcp range: restart_delay_max_s 600 is the high "
			"bound",
		MGCP_BOUND("\toption restart_delay_max_s '600'\n",
			   restart_delay_max_s, 600, NULL));
	mgcp_bound_case("mgcp range: restart_delay_max_s 601 clamps to 600",
		MGCP_BOUND("\toption restart_delay_max_s '601'\n",
			   restart_delay_max_s, 600,
			   "voip restart_delay_max_s 601 above 600, clamped"));
	mgcp_bound_case("mgcp range: a malformed number warns once and takes "
			"the default",
		MGCP_BOUND("\toption max1 'five'\n", max1, 5,
			   "voip max1 value 'five' is not a number"));
}

/*
 * The three pairs whose low bound follows the option before them. max2 stays
 * above max1 (RFC 3435 4.3). rto_max_ms and t_hist_s stay at or above their
 * partner. A value below that bound clamps to it and warns once, and the
 * option before keeps its own value.
 */
static void mgcp_relation_tests(void)
{
	mgcp_bound_case("mgcp: max2 below max1 warns once and takes max1 + 1",
		MGCP_BOUND("\toption max1 '10'\n\toption max2 '8'\n", max2, 11,
			   "voip max2 8 below max1 + 1 (11), clamped"));
	mgcp_bound_case("mgcp: and max1 keeps its value",
		MGCP_BOUND("\toption max1 '10'\n\toption max2 '8'\n", max1, 10,
			   "voip max2 8 below max1 + 1 (11), clamped"));
	/* two options move, so two warnings: max1 clamps, max2 follows it */
	mgcp_bound_case("mgcp: max2 follows a max1 that clamped high",
		MGCP_BOUND_N("\toption max1 '25'\n", max2, 21,
			     "voip max2 7 below max1 + 1 (21), clamped", 2));
	mgcp_bound_case("mgcp: rto_max_ms below rto_init_ms warns once and "
			"takes rto_init_ms",
		MGCP_BOUND("\toption rto_init_ms '1000'\n"
			   "\toption rto_max_ms '500'\n", rto_max_ms, 1000,
			   "voip rto_max_ms 500 below rto_init_ms (1000), "
			   "clamped"));
	mgcp_bound_case("mgcp: t_hist_s below t_max_s warns once and takes "
			"t_max_s",
		MGCP_BOUND("\toption t_max_s '60'\n\toption t_hist_s '40'\n",
			   t_hist_s, 60,
			   "voip t_hist_s 40 below t_max_s (60), clamped"));
}

/*
 * endpoint_pattern: {line} and {host} each appear once at most, and no
 * other brace appears. A pattern the rule refuses, or one the field cannot
 * hold whole, warns once and takes the default.
 */
static void mgcp_pattern_tests(void)
{
	static const char def_warn[] =
		"is invalid, using default 'aaln/{line}@{host}'";
	struct cfg_fixture fixture;
	char option[160];
	char text[CFG_STR_MAX + 1];

	CASE("mgcp pattern: a pattern with both tokens loads");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, mgcp_option_load(
		"\toption endpoint_pattern '{host}/aaln{line}'\n"));
	CHECK_CSTR(fixture.cfg->mgcp.endpoint_pattern, "{host}/aaln{line}");
	CHECK_UINT(warn_capture_count("endpoint_pattern"), 0);
	warn_capture_end();
	fixture_free(&fixture);

	CASE("mgcp pattern: a pattern with no token loads");
	FIXTURE_LOAD_OR_RETURN(fixture, mgcp_option_load(
		"\toption endpoint_pattern 'aaln/1@gw.example.test'\n"));
	CHECK_CSTR(fixture.cfg->mgcp.endpoint_pattern,
		   "aaln/1@gw.example.test");
	fixture_free(&fixture);

	CASE("mgcp pattern: a pattern without the host token loads");
	FIXTURE_LOAD_OR_RETURN(fixture, mgcp_option_load(
		"\toption endpoint_pattern 'aaln/{line}'\n"));
	CHECK_CSTR(fixture.cfg->mgcp.endpoint_pattern, "aaln/{line}");
	fixture_free(&fixture);

	CASE("mgcp pattern: a repeated token warns once and takes the default");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, mgcp_option_load(
		"\toption endpoint_pattern '{line}/{line}@{host}'\n"));
	CHECK_CSTR(fixture.cfg->mgcp.endpoint_pattern, "aaln/{line}@{host}");
	CHECK_UINT(warn_capture_count(
		"voip mgcp endpoint_pattern value '{line}/{line}@{host}' "
		"is invalid"), 1);
	warn_capture_end();
	fixture_free(&fixture);

	CASE("mgcp pattern: a repeated host token warns once and takes the "
	     "default");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, mgcp_option_load(
		"\toption endpoint_pattern '{host}/aaln{line}@{host}'\n"));
	CHECK_CSTR(fixture.cfg->mgcp.endpoint_pattern, "aaln/{line}@{host}");
	CHECK_UINT(warn_capture_count(
		"voip mgcp endpoint_pattern value '{host}/aaln{line}@{host}' "
		"is invalid"), 1);
	warn_capture_end();
	fixture_free(&fixture);

	CASE("mgcp pattern: an unknown token warns once and takes the default");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, mgcp_option_load(
		"\toption endpoint_pattern 'aaln/{port}@{host}'\n"));
	CHECK_CSTR(fixture.cfg->mgcp.endpoint_pattern, "aaln/{line}@{host}");
	CHECK_UINT(warn_capture_count(def_warn), 1);
	warn_capture_end();
	fixture_free(&fixture);

	CASE("mgcp pattern: an open brace warns once and takes the default");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, mgcp_option_load(
		"\toption endpoint_pattern 'aaln/{line@{host}'\n"));
	CHECK_CSTR(fixture.cfg->mgcp.endpoint_pattern, "aaln/{line}@{host}");
	CHECK_UINT(warn_capture_count(def_warn), 1);
	warn_capture_end();
	fixture_free(&fixture);

	CASE("mgcp pattern: a close brace alone warns once and takes the "
	     "default");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, mgcp_option_load(
		"\toption endpoint_pattern 'aaln/1}@{host}'\n"));
	CHECK_CSTR(fixture.cfg->mgcp.endpoint_pattern, "aaln/{line}@{host}");
	CHECK_UINT(warn_capture_count(def_warn), 1);
	warn_capture_end();
	fixture_free(&fixture);

	CASE("mgcp pattern: a pattern of 63 characters loads");
	memset(text, 'a', CFG_STR_MAX - 1);
	text[CFG_STR_MAX - 1] = '\0';
	snprintf(option, sizeof(option), "\toption endpoint_pattern '%s'\n",
		 text);
	FIXTURE_LOAD_OR_RETURN(fixture, mgcp_option_load(option));
	CHECK_CSTR(fixture.cfg->mgcp.endpoint_pattern, text);
	fixture_free(&fixture);

	CASE("mgcp pattern: an overlong pattern warns once and takes the "
	     "default");
	memset(text, 'a', CFG_STR_MAX);
	text[CFG_STR_MAX] = '\0';
	snprintf(option, sizeof(option), "\toption endpoint_pattern '%s'\n",
		 text);
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, mgcp_option_load(option));
	CHECK_CSTR(fixture.cfg->mgcp.endpoint_pattern, "aaln/{line}@{host}");
	CHECK_UINT(warn_capture_count(def_warn), 1);
	warn_capture_end();
	fixture_free(&fixture);

	CASE("mgcp pattern: an empty pattern is dropped by libuci and loads "
	     "the default");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture,
		mgcp_option_load("\toption endpoint_pattern ''\n"));
	CHECK_CSTR(fixture.cfg->mgcp.endpoint_pattern, "aaln/{line}@{host}");
	CHECK_UINT(warn_capture_count("endpoint_pattern"), 0);
	warn_capture_end();
	fixture_free(&fixture);
}

/* One persistent_events value that the grammar refuses. */
static void mgcp_events_bad_case(const char *name, const char *value)
{
	struct cfg_fixture fixture;
	char option[160];
	char needle[160];

	CASE(name);
	snprintf(option, sizeof(option), "\toption persistent_events '%s'\n",
		 value);
	snprintf(needle, sizeof(needle),
		 "voip mgcp persistent_events value '%s' is invalid, using "
		 "default 'hd,hu,hf'", value);
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, mgcp_option_load(option));
	CHECK_CSTR(fixture.cfg->mgcp.persistent_events, "hd,hu,hf");
	CHECK_UINT(warn_capture_count(needle), 1);
	warn_capture_end();
	fixture_free(&fixture);
}

/*
 * persistent_events: names of 1 to 8 characters of [a-zA-Z0-9/], with one
 * comma between two names. A list the grammar refuses, or one the field
 * cannot hold whole, warns once and takes the default. libuci drops an empty
 * value, so the literal none loads the empty list, the slic_plugin idiom.
 */
static void mgcp_events_tests(void)
{
	struct cfg_fixture fixture;
	char option[160];
	char text[CFG_STR_MAX + 1];
	unsigned int i;

	CASE("mgcp events: a list with package names loads");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, mgcp_option_load(
		"\toption persistent_events 'hd,L/hu,G/ft,abcdefgh'\n"));
	CHECK_CSTR(fixture.cfg->mgcp.persistent_events,
		   "hd,L/hu,G/ft,abcdefgh");
	CHECK_UINT(warn_capture_count("persistent_events"), 0);
	warn_capture_end();
	fixture_free(&fixture);

	CASE("mgcp events: one name loads");
	FIXTURE_LOAD_OR_RETURN(fixture,
		mgcp_option_load("\toption persistent_events 'hf'\n"));
	CHECK_CSTR(fixture.cfg->mgcp.persistent_events, "hf");
	fixture_free(&fixture);

	CASE("mgcp events: an empty list is dropped by libuci and loads the "
	     "default");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture,
		mgcp_option_load("\toption persistent_events ''\n"));
	CHECK_CSTR(fixture.cfg->mgcp.persistent_events, "hd,hu,hf");
	CHECK_UINT(warn_capture_count("persistent_events"), 0);
	warn_capture_end();
	fixture_free(&fixture);

	CASE("mgcp events: none loads the empty list and warns not at all");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture,
		mgcp_option_load("\toption persistent_events 'none'\n"));
	CHECK_CSTR(fixture.cfg->mgcp.persistent_events, "");
	CHECK_UINT(warn_capture_count("persistent_events"), 0);
	warn_capture_end();
	fixture_free(&fixture);

	CASE("mgcp events: NONE is an event name, not the sentinel");
	FIXTURE_LOAD_OR_RETURN(fixture,
		mgcp_option_load("\toption persistent_events 'NONE'\n"));
	CHECK_CSTR(fixture.cfg->mgcp.persistent_events, "NONE");
	fixture_free(&fixture);

	CASE("mgcp events: a list form reads as absent and loads the default");
	FIXTURE_LOAD_OR_RETURN(fixture, mgcp_option_load(
		"\tlist persistent_events 'hd'\n"
		"\tlist persistent_events 'hu'\n"));
	CHECK_CSTR(fixture.cfg->mgcp.persistent_events, "hd,hu,hf");
	fixture_free(&fixture);

	mgcp_events_bad_case("mgcp events: a space after a comma warns once "
			     "and takes the default", "hd, hu");
	mgcp_events_bad_case("mgcp events: a space alone warns once and takes "
			     "the default", " ");
	mgcp_events_bad_case("mgcp events: a bad character warns once and "
			     "takes the default", "hd;hu");
	mgcp_events_bad_case("mgcp events: a name of nine characters warns "
			     "once and takes the default", "abcdefghi");
	mgcp_events_bad_case("mgcp events: two commas warn once and take the "
			     "default", "hd,,hu");
	mgcp_events_bad_case("mgcp events: a leading comma warns once and "
			     "takes the default", ",hd");
	mgcp_events_bad_case("mgcp events: a trailing comma warns once and "
			     "takes the default", "hd,");

	/* "hhh,hh,hh,...": every name is valid, so only the length decides */
	CASE("mgcp events: a list of 63 characters loads");
	for (i = 0; i < CFG_STR_MAX - 1; i++)
		text[i] = i && i % 3 == 0 ? ',' : 'h';
	text[CFG_STR_MAX - 1] = '\0';
	snprintf(option, sizeof(option), "\toption persistent_events '%s'\n",
		 text);
	FIXTURE_LOAD_OR_RETURN(fixture, mgcp_option_load(option));
	CHECK_CSTR(fixture.cfg->mgcp.persistent_events, text);
	fixture_free(&fixture);

	CASE("mgcp events: an overlong list warns once and takes the default");
	for (i = 0; i < CFG_STR_MAX; i++)
		text[i] = i && i % 3 == 0 ? ',' : 'h';
	text[CFG_STR_MAX - 1] = 'h';
	text[CFG_STR_MAX] = '\0';
	snprintf(option, sizeof(option), "\toption persistent_events '%s'\n",
		 text);
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, mgcp_option_load(option));
	CHECK_CSTR(fixture.cfg->mgcp.persistent_events, "hd,hu,hf");
	CHECK_UINT(warn_capture_count(
		"persistent_events value 'hhh,hh,"), 1);
	warn_capture_end();
	fixture_free(&fixture);
}

enum {
	MGCP_DUMP_MGCP,
	MGCP_DUMP_MAX,
};

enum {
	MGCP_CALL_AGENT,
	MGCP_CALL_AGENT_PORT,
	MGCP_LOCAL_PORT,
	MGCP_DOMAIN,
	MGCP_BRACKETED_IP,
	MGCP_ENDPOINT_PATTERN,
	MGCP_LINE_BASE,
	MGCP_DIALECT,
	MGCP_MAX1,
	MGCP_MAX2,
	MGCP_RTO_INIT_MS,
	MGCP_RTO_MAX_MS,
	MGCP_T_MAX_S,
	MGCP_T_HIST_S,
	MGCP_PERSISTENT_EVENTS,
	MGCP_QUARANTINE_DEFAULT,
	MGCP_SEND_PTIME,
	MGCP_REPLY_METHOD,
	MGCP_SIGNALING_DSCP,
	MGCP_RESTART_DELAY_MAX_S,
	MGCP_KEY_MAX,
};

static const struct blobmsg_policy mgcp_dump_policy[MGCP_DUMP_MAX] = {
	[MGCP_DUMP_MGCP] = { .name = "mgcp", .type = BLOBMSG_TYPE_TABLE },
};

#define MGCP_KEY(id, key, kind) \
	[id] = { .name = key, .type = BLOBMSG_TYPE_##kind }

static const struct blobmsg_policy mgcp_key_policy[MGCP_KEY_MAX] = {
	MGCP_KEY(MGCP_CALL_AGENT, "call_agent", STRING),
	MGCP_KEY(MGCP_CALL_AGENT_PORT, "call_agent_port", INT32),
	MGCP_KEY(MGCP_LOCAL_PORT, "local_port", INT32),
	MGCP_KEY(MGCP_DOMAIN, "domain", STRING),
	MGCP_KEY(MGCP_BRACKETED_IP, "bracketed_ip", BOOL),
	MGCP_KEY(MGCP_ENDPOINT_PATTERN, "endpoint_pattern", STRING),
	MGCP_KEY(MGCP_LINE_BASE, "line_base", INT32),
	MGCP_KEY(MGCP_DIALECT, "dialect", STRING),
	MGCP_KEY(MGCP_MAX1, "max1", INT32),
	MGCP_KEY(MGCP_MAX2, "max2", INT32),
	MGCP_KEY(MGCP_RTO_INIT_MS, "rto_init_ms", INT32),
	MGCP_KEY(MGCP_RTO_MAX_MS, "rto_max_ms", INT32),
	MGCP_KEY(MGCP_T_MAX_S, "t_max_s", INT32),
	MGCP_KEY(MGCP_T_HIST_S, "t_hist_s", INT32),
	MGCP_KEY(MGCP_PERSISTENT_EVENTS, "persistent_events", STRING),
	MGCP_KEY(MGCP_QUARANTINE_DEFAULT, "quarantine_default", STRING),
	MGCP_KEY(MGCP_SEND_PTIME, "send_ptime", BOOL),
	MGCP_KEY(MGCP_REPLY_METHOD, "reply_method", STRING),
	MGCP_KEY(MGCP_SIGNALING_DSCP, "signaling_dscp", INT32),
	MGCP_KEY(MGCP_RESTART_DELAY_MAX_S, "restart_delay_max_s", INT32),
};

#undef MGCP_KEY

static bool mgcp_dump_get(const struct cfg *cfg, struct blob_buf *buf,
			  struct blob_attr **keys)
{
	struct blob_attr *dump_fields[MGCP_DUMP_MAX] = { 0 };

	blobmsg_buf_init(buf);
	CHECK(buf->head != NULL);
	if (!buf->head)
		return false;

	cfg_dump(cfg, buf);
	blobmsg_parse_attr(mgcp_dump_policy, MGCP_DUMP_MAX, dump_fields,
			   buf->head);
	CHECK(dump_fields[MGCP_DUMP_MGCP] != NULL);
	if (dump_fields[MGCP_DUMP_MGCP])
		blobmsg_parse_attr(mgcp_key_policy, MGCP_KEY_MAX, keys,
				   dump_fields[MGCP_DUMP_MGCP]);

	return true;
}

static void mgcp_dump_str_check(struct blob_attr *attr, const char *want)
{
	CHECK(attr != NULL);
	if (attr)
		CHECK_CSTR(blobmsg_get_string(attr), want);
}

static void mgcp_dump_u32_check(struct blob_attr *attr, unsigned int want)
{
	CHECK(attr != NULL);
	if (attr)
		CHECK_UINT(blobmsg_get_u32(attr), want);
}

static void mgcp_dump_bool_check(struct blob_attr *attr, bool want)
{
	CHECK(attr != NULL);
	if (attr)
		CHECK(blobmsg_get_bool(attr) == want);
}

/*
 * The dump nests the section under "mgcp", with every option on its uci
 * name. The enum rows report their word. No mgcp option is a credential.
 */
static void mgcp_dump_tests(void)
{
	struct blob_attr *keys[MGCP_KEY_MAX] = { 0 };
	struct cfg_fixture fixture;
	struct blob_buf buf = { 0 };

	CASE("mgcp dump: the dump carries every option the config set");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(mgcp_full_config));
	if (!mgcp_dump_get(fixture.cfg, &buf, keys)) {
		fixture_free(&fixture);
		return;
	}
	mgcp_dump_str_check(keys[MGCP_CALL_AGENT], "ca.example.test");
	mgcp_dump_u32_check(keys[MGCP_CALL_AGENT_PORT], 2728);
	mgcp_dump_u32_check(keys[MGCP_LOCAL_PORT], 2429);
	mgcp_dump_str_check(keys[MGCP_DOMAIN], "gw.example.test");
	mgcp_dump_bool_check(keys[MGCP_BRACKETED_IP], true);
	mgcp_dump_str_check(keys[MGCP_ENDPOINT_PATTERN], "ep/{line}@{host}");
	mgcp_dump_u32_check(keys[MGCP_LINE_BASE], 0);
	mgcp_dump_str_check(keys[MGCP_DIALECT], "ncs");
	mgcp_dump_u32_check(keys[MGCP_MAX1], 4);
	mgcp_dump_u32_check(keys[MGCP_MAX2], 9);
	mgcp_dump_u32_check(keys[MGCP_RTO_INIT_MS], 250);
	mgcp_dump_u32_check(keys[MGCP_RTO_MAX_MS], 8000);
	mgcp_dump_u32_check(keys[MGCP_T_MAX_S], 25);
	mgcp_dump_u32_check(keys[MGCP_T_HIST_S], 40);
	mgcp_dump_str_check(keys[MGCP_PERSISTENT_EVENTS], "hd,L/hu");
	mgcp_dump_str_check(keys[MGCP_QUARANTINE_DEFAULT], "loop");
	mgcp_dump_bool_check(keys[MGCP_SEND_PTIME], false);
	mgcp_dump_str_check(keys[MGCP_REPLY_METHOD], "mgc_port");
	mgcp_dump_u32_check(keys[MGCP_SIGNALING_DSCP], 46);
	mgcp_dump_u32_check(keys[MGCP_RESTART_DELAY_MAX_S], 600);
	blob_buf_free(&buf);
	fixture_free(&fixture);

	CASE("mgcp dump: the dump reports the defaults of an absent section");
	memset(keys, 0, sizeof(keys));
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load("config base 'base'\n"));
	if (!mgcp_dump_get(fixture.cfg, &buf, keys)) {
		fixture_free(&fixture);
		return;
	}
	mgcp_dump_str_check(keys[MGCP_CALL_AGENT], "");
	mgcp_dump_u32_check(keys[MGCP_CALL_AGENT_PORT], 2727);
	mgcp_dump_u32_check(keys[MGCP_LOCAL_PORT], 2427);
	mgcp_dump_str_check(keys[MGCP_DOMAIN], "");
	mgcp_dump_bool_check(keys[MGCP_BRACKETED_IP], false);
	mgcp_dump_str_check(keys[MGCP_ENDPOINT_PATTERN], "aaln/{line}@{host}");
	mgcp_dump_u32_check(keys[MGCP_LINE_BASE], 1);
	mgcp_dump_str_check(keys[MGCP_DIALECT], "rfc3435");
	mgcp_dump_u32_check(keys[MGCP_MAX1], 5);
	mgcp_dump_u32_check(keys[MGCP_MAX2], 7);
	mgcp_dump_u32_check(keys[MGCP_RTO_INIT_MS], 200);
	mgcp_dump_u32_check(keys[MGCP_RTO_MAX_MS], 4000);
	mgcp_dump_u32_check(keys[MGCP_T_MAX_S], 20);
	mgcp_dump_u32_check(keys[MGCP_T_HIST_S], 30);
	mgcp_dump_str_check(keys[MGCP_PERSISTENT_EVENTS], "hd,hu,hf");
	mgcp_dump_str_check(keys[MGCP_QUARANTINE_DEFAULT], "step");
	mgcp_dump_bool_check(keys[MGCP_SEND_PTIME], true);
	mgcp_dump_str_check(keys[MGCP_REPLY_METHOD], "source");
	mgcp_dump_u32_check(keys[MGCP_SIGNALING_DSCP], 24);
	mgcp_dump_u32_check(keys[MGCP_RESTART_DELAY_MAX_S], 10);
	blob_buf_free(&buf);
	fixture_free(&fixture);

	/* the slic_plugin idiom: none stores empty, and the dump says so */
	CASE("mgcp dump: a persistent_events of none dumps as the empty list");
	memset(keys, 0, sizeof(keys));
	FIXTURE_LOAD_OR_RETURN(fixture,
		mgcp_option_load("\toption persistent_events 'none'\n"));
	if (!mgcp_dump_get(fixture.cfg, &buf, keys)) {
		fixture_free(&fixture);
		return;
	}
	mgcp_dump_str_check(keys[MGCP_PERSISTENT_EVENTS], "");
	blob_buf_free(&buf);
	fixture_free(&fixture);
}

/* One host option of a given length: it loads whole or loads empty. */
static void mgcp_host_case(const char *name, const char *opt, size_t len)
{
	struct cfg_fixture fixture;
	char option[CFG_MGCP_DOMAIN_MAX + 64];
	char text[CFG_MGCP_DOMAIN_MAX + 1];
	char needle[64];
	const char *got;
	bool fits;

	CASE(name);
	memset(text, 'h', len);
	text[len] = '\0';
	snprintf(option, sizeof(option), "\toption %s '%s'\n", opt, text);
	snprintf(needle, sizeof(needle), "voip mgcp %s value 'hhh", opt);
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, mgcp_option_load(option));
	if (!strcmp(opt, "call_agent")) {
		got = fixture.cfg->mgcp.call_agent;
		fits = len < sizeof(fixture.cfg->mgcp.call_agent);
	} else {
		got = fixture.cfg->mgcp.domain;
		fits = len < sizeof(fixture.cfg->mgcp.domain);
	}
	CHECK_CSTR(got, fits ? text : "");
	CHECK_UINT(warn_capture_count(needle), fits ? 0 : 1);
	warn_capture_end();
	fixture_free(&fixture);
}

/*
 * call_agent and domain load whole or not at all. A host that the field
 * cannot hold warns once and loads the empty default. Reason: a cut host
 * names another host. The none sentinel belongs to persistent_events alone,
 * so a host of that text loads as written.
 */
static void mgcp_host_tests(void)
{
	struct cfg_fixture fixture;

	CASE("mgcp host: a call_agent of none loads the literal text");
	FIXTURE_LOAD_OR_RETURN(fixture, mgcp_option_load(
		"\toption call_agent 'none'\n\toption domain 'none'\n"));
	CHECK_CSTR(fixture.cfg->mgcp.call_agent, "none");
	CHECK_CSTR(fixture.cfg->mgcp.domain, "none");
	fixture_free(&fixture);

	mgcp_host_case("mgcp host: a call_agent of 63 characters loads",
		       "call_agent", CFG_STR_MAX - 1);
	mgcp_host_case("mgcp host: a call_agent of 64 characters warns once "
		       "and loads empty", "call_agent", CFG_STR_MAX);
	mgcp_host_case("mgcp host: a domain of 127 characters loads",
		       "domain", CFG_MGCP_DOMAIN_MAX - 1);
	mgcp_host_case("mgcp host: a domain of 128 characters warns once and "
		       "loads empty", "domain", CFG_MGCP_DOMAIN_MAX);
}

/*
 * The base SRTP switch. The loader carries it. The call layer reads it at the
 * start of a leg, with the backend feature bit (call.c, HAL_FEAT_SRTP). The
 * offer and the answer of RTP/SAVP in sdp.c follow from that.
 */
static void srtp_tests(void)
{
	struct cfg_fixture fixture;

	CASE("srtp_enabled '1' loads true");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(srtp_config));
	CHECK(fixture.cfg->srtp_enabled);
	fixture_free(&fixture);

	CASE("an absent srtp_enabled loads the schema default");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n\toption channels '1'\n"));
	CHECK(!fixture.cfg->srtp_enabled);
	fixture_free(&fixture);

	/* opt_bool() reads text it refuses as an absent option: one warning and
	 * the schema default, the rule every other base bool follows. */
	CASE("a malformed srtp_enabled warns once and reads as unset");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n"
		"\toption channels '1'\n"
		"\toption srtp_enabled 'maybe'\n"));
	CHECK(!fixture.cfg->srtp_enabled);
	CHECK_UINT(warn_capture_count(
		"voip srtp_enabled value 'maybe' is not a boolean, treated as unset"), 1);
	fixture_free(&fixture);

	/*
	 * The placement of the load is the contract here, the rule the five
	 * TLS options follow. A channel count the loader cannot read disables
	 * voice and returns, and srtp_enabled is read ahead of that return, so
	 * the fault still reports the switch an operator configured. A load
	 * moved below the fault return fails this case.
	 */
	CASE("the channels fault leaves srtp_enabled loaded");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n"
		"\toption enabled '1'\n"
		"\toption channels 'abc'\n"
		"\toption srtp_enabled '1'\n"));
	CHECK(!fixture.cfg->enabled);
	CHECK_UINT(fixture.cfg->channels, 0);
	CHECK(fixture.cfg->srtp_enabled);
	CHECK_UINT(warn_capture_count(
		"voip.base.channels 'abc' is not a number, voice disabled"), 1);
	fixture_free(&fixture);
	warn_capture_end();
}

/*
 * The support class of srtp_enabled. Nothing in the daemon reads enum
 * cfg_support, so the descriptor table is the only place that holds the
 * answer. A row that read accepted-unsupported would tell an operator that
 * the daemon ignores an option that the loader carries.
 */
static void srtp_support_test(void)
{
	const struct cfg_option_desc *d;
	unsigned int found = 0;
	unsigned int i;

	CASE("schema: srtp_enabled is supported and restarts all");
	for (i = 0; cfg_base_options[i].name; i++) {
		d = &cfg_base_options[i];
		if (strcmp(d->name, "srtp_enabled"))
			continue;

		found++;
		CHECK_UINT(d->support, SUP_SUPPORTED);
		CHECK_UINT(d->apply_class, APPLY_RESTART_ALL);
	}
	CHECK_UINT(found, 1);
}

/*
 * The support class of prack, by the rule that srtp_enabled states above.
 * call.c reads the option on every inbound INVITE that offers 100rel. A row
 * that read accepted-unsupported would tell an operator that the daemon
 * ignores it.
 */
static void prack_support_test(void)
{
	const struct cfg_option_desc *d;
	unsigned int found = 0;
	unsigned int i;

	CASE("schema: prack is a supported bool that applies live");
	for (i = 0; cfg_channel_options[i].name; i++) {
		d = &cfg_channel_options[i];
		if (strcmp(d->name, "prack"))
			continue;

		found++;
		CHECK_UINT(d->type, CFG_BOOL);
		CHECK(d->def != NULL);
		if (d->def)
			CHECK_CSTR(d->def, "0");
		CHECK_UINT(d->support, SUP_SUPPORTED);
		CHECK_UINT(d->apply_class, APPLY_LIVE);
	}
	CHECK_UINT(found, 1);
}

/* The switch is no credential, so the diagnostic dump carries it. */
static void srtp_dump_tests(void)
{
	enum {
		DUMP_SRTP_ENABLED,
		DUMP_MAX,
	};
	static const struct blobmsg_policy dump_policy[DUMP_MAX] = {
		[DUMP_SRTP_ENABLED] = {
			.name = "srtp_enabled",
			.type = BLOBMSG_TYPE_BOOL,
		},
	};
	struct blob_attr *dump_fields[DUMP_MAX] = { 0 };
	struct cfg_fixture fixture;
	struct blob_buf buf = { 0 };

	CASE("the dump carries srtp_enabled when the config sets it");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(srtp_config));
	blobmsg_buf_init(&buf);
	CHECK(buf.head != NULL);
	if (!buf.head) {
		blob_buf_free(&buf);
		fixture_free(&fixture);
		return;
	}

	cfg_dump(fixture.cfg, &buf);
	blobmsg_parse_attr(dump_policy, DUMP_MAX, dump_fields, buf.head);
	CHECK(dump_fields[DUMP_SRTP_ENABLED] != NULL);
	if (dump_fields[DUMP_SRTP_ENABLED])
		CHECK(blobmsg_get_bool(dump_fields[DUMP_SRTP_ENABLED]));

	blob_buf_free(&buf);
	fixture_free(&fixture);

	CASE("the dump reports a defaulted srtp_enabled as off");
	memset(dump_fields, 0, sizeof(dump_fields));
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n\toption channels '1'\n"));
	blobmsg_buf_init(&buf);
	CHECK(buf.head != NULL);
	if (!buf.head) {
		blob_buf_free(&buf);
		fixture_free(&fixture);
		return;
	}

	cfg_dump(fixture.cfg, &buf);
	blobmsg_parse_attr(dump_policy, DUMP_MAX, dump_fields, buf.head);
	CHECK(dump_fields[DUMP_SRTP_ENABLED] != NULL);
	if (dump_fields[DUMP_SRTP_ENABLED])
		CHECK(!blobmsg_get_bool(dump_fields[DUMP_SRTP_ENABLED]));

	blob_buf_free(&buf);
	fixture_free(&fixture);
}

/* ipv6_enabled is the operator gate; nothing in this file consumes it. */
static void ipv6_tests(void)
{
	struct cfg_fixture fixture;

	CASE("ipv6_enabled '1' loads true");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n\toption channels '1'\n"
		"\toption ipv6_enabled '1'\n"));
	CHECK(fixture.cfg->ipv6_enabled);
	fixture_free(&fixture);

	CASE("an absent ipv6_enabled loads the schema default");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n\toption channels '1'\n"));
	CHECK(!fixture.cfg->ipv6_enabled);
	fixture_free(&fixture);

	CASE("a malformed ipv6_enabled warns once and reads as unset");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n\toption channels '1'\n"
		"\toption ipv6_enabled 'maybe'\n"));
	CHECK(!fixture.cfg->ipv6_enabled);
	CHECK_UINT(warn_capture_count(
		"voip ipv6_enabled value 'maybe' is not a boolean, "
		"treated as unset"), 1);
	CHECK_UINT(warn_capture_count("ipv6_enabled"), 1);
	fixture_free(&fixture);

	/* A load moved below the fault return fails this case. */
	CASE("the channels fault leaves ipv6_enabled loaded");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n"
		"\toption enabled '1'\n"
		"\toption channels 'abc'\n"
		"\toption ipv6_enabled '1'\n"));
	CHECK(!fixture.cfg->enabled);
	CHECK_UINT(fixture.cfg->channels, 0);
	CHECK(fixture.cfg->ipv6_enabled);
	CHECK_UINT(warn_capture_count(
		"voip.base.channels 'abc' is not a number, voice disabled"), 1);
	fixture_free(&fixture);
	warn_capture_end();
}

static void ipv6_support_test(void)
{
	const struct cfg_option_desc *d;
	unsigned int found = 0;
	unsigned int i;

	CASE("schema: ipv6_enabled is supported and restarts all");
	for (i = 0; cfg_base_options[i].name; i++) {
		d = &cfg_base_options[i];
		if (strcmp(d->name, "ipv6_enabled"))
			continue;

		found++;
		CHECK_UINT(d->support, SUP_SUPPORTED);
		CHECK_UINT(d->apply_class, APPLY_RESTART_ALL);
	}
	CHECK_UINT(found, 1);
}

static void ipv6_dump_tests(void)
{
	enum {
		DUMP_IPV6_ENABLED,
		DUMP_MAX,
	};
	static const struct blobmsg_policy dump_policy[DUMP_MAX] = {
		[DUMP_IPV6_ENABLED] = {
			.name = "ipv6_enabled",
			.type = BLOBMSG_TYPE_BOOL,
		},
	};
	struct blob_attr *dump_fields[DUMP_MAX] = { 0 };
	struct cfg_fixture fixture;
	struct blob_buf buf = { 0 };

	CASE("the dump carries ipv6_enabled");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n\toption channels '1'\n"
		"\toption ipv6_enabled '1'\n"));
	blobmsg_buf_init(&buf);
	CHECK(buf.head != NULL);
	if (!buf.head) {
		blob_buf_free(&buf);
		fixture_free(&fixture);
		return;
	}

	cfg_dump(fixture.cfg, &buf);
	blobmsg_parse_attr(dump_policy, DUMP_MAX, dump_fields, buf.head);
	CHECK(dump_fields[DUMP_IPV6_ENABLED] != NULL);
	if (dump_fields[DUMP_IPV6_ENABLED])
		CHECK(blobmsg_get_bool(dump_fields[DUMP_IPV6_ENABLED]));

	blob_buf_free(&buf);
	fixture_free(&fixture);

	CASE("the dump reports a defaulted ipv6_enabled as off");
	memset(dump_fields, 0, sizeof(dump_fields));
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n\toption channels '1'\n"));
	blobmsg_buf_init(&buf);
	CHECK(buf.head != NULL);
	if (!buf.head) {
		blob_buf_free(&buf);
		fixture_free(&fixture);
		return;
	}

	cfg_dump(fixture.cfg, &buf);
	blobmsg_parse_attr(dump_policy, DUMP_MAX, dump_fields, buf.head);
	CHECK(dump_fields[DUMP_IPV6_ENABLED] != NULL);
	if (dump_fields[DUMP_IPV6_ENABLED])
		CHECK(!blobmsg_get_bool(dump_fields[DUMP_IPV6_ENABLED]));

	blob_buf_free(&buf);
	fixture_free(&fixture);
}

/*
 * The four base options that the dump carries. Each one is a loaded field of
 * struct cfg that holds no credential, which is what cfg.h says the
 * diagnostic dump carries. Each one shapes what an operator sees on the wire:
 * the refresh interval of a NAT pinhole, the OPTIONS ping and the solicited
 * MWI subscription. The cases check a configured value and the schema default
 * per key. A dump that reported a constant would look correct for a default
 * config.
 */
static void base_dump_tests(void)
{
	enum {
		DUMP_KEEPALIVE_ENABLED,
		DUMP_KEEPALIVE_INTERVAL,
		DUMP_OPTIONS_INTERVAL,
		DUMP_MWI_SUBSCRIBE,
		DUMP_MAX,
	};
	static const struct blobmsg_policy dump_policy[DUMP_MAX] = {
		[DUMP_KEEPALIVE_ENABLED] = {
			.name = "keepalive_enabled",
			.type = BLOBMSG_TYPE_BOOL,
		},
		[DUMP_KEEPALIVE_INTERVAL] = {
			.name = "keepalive_interval",
			.type = BLOBMSG_TYPE_INT32,
		},
		[DUMP_OPTIONS_INTERVAL] = {
			.name = "options_interval",
			.type = BLOBMSG_TYPE_INT32,
		},
		[DUMP_MWI_SUBSCRIBE] = {
			.name = "mwi_subscribe",
			.type = BLOBMSG_TYPE_BOOL,
		},
	};
	struct blob_attr *dump_fields[DUMP_MAX] = { 0 };
	struct cfg_fixture fixture;
	struct blob_buf buf = { 0 };

	CASE("the dump carries the four base options the config sets");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n"
		"\toption channels '1'\n"
		"\toption keepalive_enabled '0'\n"
		"\toption keepalive_interval '30'\n"
		"\toption options_interval '45'\n"
		"\toption mwi_subscribe '1'\n"));
	blobmsg_buf_init(&buf);
	CHECK(buf.head != NULL);
	if (!buf.head) {
		blob_buf_free(&buf);
		fixture_free(&fixture);
		return;
	}

	cfg_dump(fixture.cfg, &buf);
	blobmsg_parse_attr(dump_policy, DUMP_MAX, dump_fields, buf.head);
	CHECK(dump_fields[DUMP_KEEPALIVE_ENABLED] != NULL);
	CHECK(dump_fields[DUMP_KEEPALIVE_INTERVAL] != NULL);
	CHECK(dump_fields[DUMP_OPTIONS_INTERVAL] != NULL);
	CHECK(dump_fields[DUMP_MWI_SUBSCRIBE] != NULL);
	if (dump_fields[DUMP_KEEPALIVE_ENABLED])
		CHECK(!blobmsg_get_bool(dump_fields[DUMP_KEEPALIVE_ENABLED]));
	if (dump_fields[DUMP_KEEPALIVE_INTERVAL])
		CHECK_UINT(blobmsg_get_u32(dump_fields[DUMP_KEEPALIVE_INTERVAL]), 30);
	if (dump_fields[DUMP_OPTIONS_INTERVAL])
		CHECK_UINT(blobmsg_get_u32(dump_fields[DUMP_OPTIONS_INTERVAL]), 45);
	if (dump_fields[DUMP_MWI_SUBSCRIBE])
		CHECK(blobmsg_get_bool(dump_fields[DUMP_MWI_SUBSCRIBE]));

	blob_buf_free(&buf);
	fixture_free(&fixture);

	CASE("the dump reports the schema default of each of the four");
	memset(dump_fields, 0, sizeof(dump_fields));
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n\toption channels '1'\n"));
	blobmsg_buf_init(&buf);
	CHECK(buf.head != NULL);
	if (!buf.head) {
		blob_buf_free(&buf);
		fixture_free(&fixture);
		return;
	}

	cfg_dump(fixture.cfg, &buf);
	blobmsg_parse_attr(dump_policy, DUMP_MAX, dump_fields, buf.head);
	CHECK(dump_fields[DUMP_KEEPALIVE_ENABLED] != NULL);
	CHECK(dump_fields[DUMP_KEEPALIVE_INTERVAL] != NULL);
	CHECK(dump_fields[DUMP_OPTIONS_INTERVAL] != NULL);
	CHECK(dump_fields[DUMP_MWI_SUBSCRIBE] != NULL);
	if (dump_fields[DUMP_KEEPALIVE_ENABLED])
		CHECK(blobmsg_get_bool(dump_fields[DUMP_KEEPALIVE_ENABLED]));
	if (dump_fields[DUMP_KEEPALIVE_INTERVAL])
		CHECK_UINT(blobmsg_get_u32(dump_fields[DUMP_KEEPALIVE_INTERVAL]), 30);
	if (dump_fields[DUMP_OPTIONS_INTERVAL])
		CHECK_UINT(blobmsg_get_u32(dump_fields[DUMP_OPTIONS_INTERVAL]), 0);
	if (dump_fields[DUMP_MWI_SUBSCRIBE])
		CHECK(!blobmsg_get_bool(dump_fields[DUMP_MWI_SUBSCRIBE]));

	blob_buf_free(&buf);
	fixture_free(&fixture);
}

enum {
	DUMP_CHANNEL,
	DUMP_MAX,
};

enum {
	CHANNEL_SIP_USERNAME,
	CHANNEL_SIP_AUTH_USERNAME,
	CHANNEL_SIP_CALLERID_NAME,
	CHANNEL_SIP_SECRET,
	CHANNEL_SIP_OUTBOUND_PROXY,
	CHANNEL_SIP_DOMAIN,
	CHANNEL_SIP_REGISTRAR,
	CHANNEL_SIP_REALM,
	CHANNEL_TRANSPORT,
	CHANNEL_LOCAL_INTERFACE,
	CHANNEL_SIP_OUTBOUND_PROXY_PORT,
	CHANNEL_SIP_REGISTRAR_PORT,
	CHANNEL_SIP_REGISTER_RETRY,
	CHANNEL_OPTIONS_INTERVAL,
	CHANNEL_G722_PTYPE,
	CHANNEL_OPUS_PTYPE,
	CHANNEL_TIMERS,
	CHANNEL_MAX,
};

enum {
	TIMER_T1,
	TIMER_T2,
	TIMER_T4,
	TIMER_A,
	TIMER_B,
	TIMER_D,
	TIMER_E,
	TIMER_F,
	TIMER_G,
	TIMER_H,
	TIMER_I,
	TIMER_J,
	TIMER_K,
	TIMER_MAX,
};

static const struct blobmsg_policy channel_dump_policy[DUMP_MAX] = {
	[DUMP_CHANNEL] = { .name = "channel", .type = BLOBMSG_TYPE_ARRAY },
};

static const struct blobmsg_policy channel_field_policy[CHANNEL_MAX] = {
	[CHANNEL_SIP_USERNAME] = {
		.name = "sip_username",
		.type = BLOBMSG_TYPE_STRING,
	},
	[CHANNEL_SIP_AUTH_USERNAME] = {
		.name = "sip_auth_username",
		.type = BLOBMSG_TYPE_STRING,
	},
	[CHANNEL_SIP_CALLERID_NAME] = {
		.name = "sip_callerid_name",
		.type = BLOBMSG_TYPE_STRING,
	},
	[CHANNEL_SIP_SECRET] = {
		.name = "sip_secret",
		.type = BLOBMSG_TYPE_STRING,
	},
	[CHANNEL_SIP_OUTBOUND_PROXY] = {
		.name = "sip_outbound_proxy",
		.type = BLOBMSG_TYPE_STRING,
	},
	[CHANNEL_SIP_DOMAIN] = {
		.name = "sip_domain",
		.type = BLOBMSG_TYPE_STRING,
	},
	[CHANNEL_SIP_REGISTRAR] = {
		.name = "sip_registrar",
		.type = BLOBMSG_TYPE_STRING,
	},
	[CHANNEL_SIP_REALM] = {
		.name = "sip_realm",
		.type = BLOBMSG_TYPE_STRING,
	},
	[CHANNEL_TRANSPORT] = {
		.name = "transport",
		.type = BLOBMSG_TYPE_STRING,
	},
	[CHANNEL_LOCAL_INTERFACE] = {
		.name = "local_interface",
		.type = BLOBMSG_TYPE_STRING,
	},
	[CHANNEL_SIP_OUTBOUND_PROXY_PORT] = {
		.name = "sip_outbound_proxy_port",
		.type = BLOBMSG_TYPE_INT32,
	},
	[CHANNEL_SIP_REGISTRAR_PORT] = {
		.name = "sip_registrar_port",
		.type = BLOBMSG_TYPE_INT32,
	},
	[CHANNEL_SIP_REGISTER_RETRY] = {
		.name = "sip_register_retry",
		.type = BLOBMSG_TYPE_INT32,
	},
	[CHANNEL_OPTIONS_INTERVAL] = {
		.name = "options_interval",
		.type = BLOBMSG_TYPE_INT32,
	},
	[CHANNEL_G722_PTYPE] = {
		.name = "g722_ptype",
		.type = BLOBMSG_TYPE_INT32,
	},
	[CHANNEL_OPUS_PTYPE] = {
		.name = "opus_ptype",
		.type = BLOBMSG_TYPE_INT32,
	},
	[CHANNEL_TIMERS] = {
		.name = "timers",
		.type = BLOBMSG_TYPE_TABLE,
	},
};

static const struct blobmsg_policy channel_timer_policy[TIMER_MAX] = {
	[TIMER_T1] = { .name = "t1", .type = BLOBMSG_TYPE_INT32 },
	[TIMER_T2] = { .name = "t2", .type = BLOBMSG_TYPE_INT32 },
	[TIMER_T4] = { .name = "t4", .type = BLOBMSG_TYPE_INT32 },
	[TIMER_A] = { .name = "a", .type = BLOBMSG_TYPE_INT32 },
	[TIMER_B] = { .name = "b", .type = BLOBMSG_TYPE_INT32 },
	[TIMER_D] = { .name = "d", .type = BLOBMSG_TYPE_INT32 },
	[TIMER_E] = { .name = "e", .type = BLOBMSG_TYPE_INT32 },
	[TIMER_F] = { .name = "f", .type = BLOBMSG_TYPE_INT32 },
	[TIMER_G] = { .name = "g", .type = BLOBMSG_TYPE_INT32 },
	[TIMER_H] = { .name = "h", .type = BLOBMSG_TYPE_INT32 },
	[TIMER_I] = { .name = "i", .type = BLOBMSG_TYPE_INT32 },
	[TIMER_J] = { .name = "j", .type = BLOBMSG_TYPE_INT32 },
	[TIMER_K] = { .name = "k", .type = BLOBMSG_TYPE_INT32 },
};

static bool channel_dump_fields_get(const struct cfg *cfg, struct blob_buf *buf,
				    struct blob_attr **channel_fields)
{
	struct blob_attr *dump_fields[DUMP_MAX] = { 0 };
	struct blob_attr *channel_entry;

	blobmsg_buf_init(buf);
	CHECK(buf->head != NULL);
	if (!buf->head)
		return false;

	cfg_dump(cfg, buf);
	blobmsg_parse_attr(channel_dump_policy, DUMP_MAX, dump_fields, buf->head);
	channel_entry = dump_fields[DUMP_CHANNEL] ?
		(struct blob_attr *)blobmsg_data(dump_fields[DUMP_CHANNEL]) : NULL;
	if (channel_entry)
		blobmsg_parse_attr(channel_field_policy, CHANNEL_MAX, channel_fields,
				   channel_entry);

	return true;
}

/* cfg_channel_options is the channel cfg-rows record. channel_dump_tests()
 * extends the base_dump_tests() rule for every non-credential option. */
static void channel_dump_tests(void)
{
	static const unsigned int configured_timers[TIMER_MAX] = {
		101, 102, 104, 201, 202, 204, 205,
		206, 207, 208, 209, 210, 211,
	};
	static const unsigned int default_timers[TIMER_MAX] = {
		500, 4000, 5000, 500, 32000, 32000, 500,
		32000, 500, 32000, 5000, 32000, 5000,
	};
	struct blob_attr *channel_fields[CHANNEL_MAX] = { 0 };
	struct blob_attr *timer_fields[TIMER_MAX] = { 0 };
	struct cfg_fixture fixture;
	struct blob_buf buf = { 0 };
	unsigned int i;

	CASE("the channel dump carries the rows it used to omit");
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(
		"\toption sip_username 'line-user'\n"
		"\toption sip_auth_username 'auth-user'\n"
		"\toption sip_callerid_name 'Caller One'\n"
		"\toption sip_secret 'private-value'\n"
		"\toption sip_outbound_proxy 'outbound.example.test'\n"
		"\toption sip_domain 'domain.example.test'\n"
		"\toption sip_registrar 'registrar.example.test'\n"
		"\toption sip_realm 'realm.example.test'\n"
		"\toption transport 'tcp'\n"
		"\toption local_interface 'wwan'\n"
		"\toption sip_outbound_proxy_port '5070'\n"
		"\toption sip_registrar_port '5080'\n"
		"\toption sip_register_retry '17'\n"
		"\toption options_interval '29'\n"
		"\toption g722_ptype '129'\n"
		"\toption opus_ptype '112'\n"
		"\toption sip_timer_t1 '101'\n"
		"\toption sip_timer_t2 '102'\n"
		"\toption sip_timer_t4 '104'\n"
		"\toption sip_timer_a '201'\n"
		"\toption sip_timer_b '202'\n"
		"\toption sip_timer_d '204'\n"
		"\toption sip_timer_e '205'\n"
		"\toption sip_timer_f '206'\n"
		"\toption sip_timer_g '207'\n"
		"\toption sip_timer_h '208'\n"
		"\toption sip_timer_i '209'\n"
		"\toption sip_timer_j '210'\n"
		"\toption sip_timer_k '211'\n"));
	if (!channel_dump_fields_get(fixture.cfg, &buf, channel_fields)) {
		blob_buf_free(&buf);
		fixture_free(&fixture);
		return;
	}
	for (i = 0; i < CHANNEL_MAX; i++) {
		if (i == CHANNEL_SIP_SECRET)
			continue;
		CHECK(channel_fields[i] != NULL);
	}
	if (channel_fields[CHANNEL_SIP_USERNAME])
		CHECK_CSTR(blobmsg_get_string(channel_fields[CHANNEL_SIP_USERNAME]),
			   "line-user");
	if (channel_fields[CHANNEL_SIP_AUTH_USERNAME])
		CHECK_CSTR(blobmsg_get_string(
			   channel_fields[CHANNEL_SIP_AUTH_USERNAME]), "auth-user");
	if (channel_fields[CHANNEL_SIP_CALLERID_NAME])
		CHECK_CSTR(blobmsg_get_string(
			   channel_fields[CHANNEL_SIP_CALLERID_NAME]), "Caller One");
	if (channel_fields[CHANNEL_SIP_OUTBOUND_PROXY])
		CHECK_CSTR(blobmsg_get_string(
			   channel_fields[CHANNEL_SIP_OUTBOUND_PROXY]),
			   "outbound.example.test");
	if (channel_fields[CHANNEL_SIP_DOMAIN])
		CHECK_CSTR(blobmsg_get_string(channel_fields[CHANNEL_SIP_DOMAIN]),
			   "domain.example.test");
	if (channel_fields[CHANNEL_SIP_REGISTRAR])
		CHECK_CSTR(blobmsg_get_string(channel_fields[CHANNEL_SIP_REGISTRAR]),
			   "registrar.example.test");
	if (channel_fields[CHANNEL_SIP_REALM])
		CHECK_CSTR(blobmsg_get_string(channel_fields[CHANNEL_SIP_REALM]),
			   "realm.example.test");
	if (channel_fields[CHANNEL_TRANSPORT])
		CHECK_CSTR(blobmsg_get_string(channel_fields[CHANNEL_TRANSPORT]),
			   "tcp");
	if (channel_fields[CHANNEL_LOCAL_INTERFACE])
		CHECK_CSTR(blobmsg_get_string(channel_fields[CHANNEL_LOCAL_INTERFACE]),
			   "wwan");
	if (channel_fields[CHANNEL_SIP_OUTBOUND_PROXY_PORT])
		CHECK_UINT(blobmsg_get_u32(
			   channel_fields[CHANNEL_SIP_OUTBOUND_PROXY_PORT]), 5070);
	if (channel_fields[CHANNEL_SIP_REGISTRAR_PORT])
		CHECK_UINT(blobmsg_get_u32(
			   channel_fields[CHANNEL_SIP_REGISTRAR_PORT]), 5080);
	if (channel_fields[CHANNEL_SIP_REGISTER_RETRY])
		CHECK_UINT(blobmsg_get_u32(
			   channel_fields[CHANNEL_SIP_REGISTER_RETRY]), 17);
	if (channel_fields[CHANNEL_OPTIONS_INTERVAL])
		CHECK_UINT(blobmsg_get_u32(channel_fields[CHANNEL_OPTIONS_INTERVAL]),
			   29);
	if (channel_fields[CHANNEL_G722_PTYPE])
		CHECK_UINT(blobmsg_get_u32(channel_fields[CHANNEL_G722_PTYPE]), 129);
	if (channel_fields[CHANNEL_OPUS_PTYPE])
		CHECK_UINT(blobmsg_get_u32(channel_fields[CHANNEL_OPUS_PTYPE]), 112);
	if (channel_fields[CHANNEL_TIMERS])
		blobmsg_parse_attr(channel_timer_policy, TIMER_MAX, timer_fields,
				   channel_fields[CHANNEL_TIMERS]);
	for (i = 0; i < TIMER_MAX; i++) {
		CHECK(timer_fields[i] != NULL);
		if (timer_fields[i])
			CHECK_UINT(blobmsg_get_u32(timer_fields[i]), configured_timers[i]);
	}
	blob_buf_free(&buf);
	fixture_free(&fixture);

	CASE("the channel dump reports the defaults of the omitted rows");
	memset(channel_fields, 0, sizeof(channel_fields));
	memset(timer_fields, 0, sizeof(timer_fields));
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(""));
	if (!channel_dump_fields_get(fixture.cfg, &buf, channel_fields)) {
		blob_buf_free(&buf);
		fixture_free(&fixture);
		return;
	}
	for (i = 0; i < CHANNEL_MAX; i++) {
		if (i == CHANNEL_SIP_SECRET)
			continue;
		CHECK(channel_fields[i] != NULL);
	}
	if (channel_fields[CHANNEL_SIP_USERNAME])
		CHECK_CSTR(blobmsg_get_string(channel_fields[CHANNEL_SIP_USERNAME]),
			   "");
	if (channel_fields[CHANNEL_SIP_AUTH_USERNAME])
		CHECK_CSTR(blobmsg_get_string(
			   channel_fields[CHANNEL_SIP_AUTH_USERNAME]), "");
	if (channel_fields[CHANNEL_SIP_CALLERID_NAME])
		CHECK_CSTR(blobmsg_get_string(
			   channel_fields[CHANNEL_SIP_CALLERID_NAME]), "");
	if (channel_fields[CHANNEL_SIP_OUTBOUND_PROXY])
		CHECK_CSTR(blobmsg_get_string(
			   channel_fields[CHANNEL_SIP_OUTBOUND_PROXY]), "");
	if (channel_fields[CHANNEL_SIP_DOMAIN])
		CHECK_CSTR(blobmsg_get_string(channel_fields[CHANNEL_SIP_DOMAIN]), "");
	if (channel_fields[CHANNEL_SIP_REGISTRAR])
		CHECK_CSTR(blobmsg_get_string(channel_fields[CHANNEL_SIP_REGISTRAR]),
			   "");
	if (channel_fields[CHANNEL_SIP_REALM])
		CHECK_CSTR(blobmsg_get_string(channel_fields[CHANNEL_SIP_REALM]), "");
	if (channel_fields[CHANNEL_TRANSPORT])
		CHECK_CSTR(blobmsg_get_string(channel_fields[CHANNEL_TRANSPORT]), "");
	if (channel_fields[CHANNEL_LOCAL_INTERFACE])
		CHECK_CSTR(blobmsg_get_string(channel_fields[CHANNEL_LOCAL_INTERFACE]),
			   "");
	if (channel_fields[CHANNEL_SIP_OUTBOUND_PROXY_PORT])
		CHECK_UINT(blobmsg_get_u32(
			   channel_fields[CHANNEL_SIP_OUTBOUND_PROXY_PORT]), 0);
	if (channel_fields[CHANNEL_SIP_REGISTRAR_PORT])
		CHECK_UINT(blobmsg_get_u32(
			   channel_fields[CHANNEL_SIP_REGISTRAR_PORT]), 0);
	if (channel_fields[CHANNEL_SIP_REGISTER_RETRY])
		CHECK_UINT(blobmsg_get_u32(
			   channel_fields[CHANNEL_SIP_REGISTER_RETRY]), 5);
	if (channel_fields[CHANNEL_OPTIONS_INTERVAL])
		CHECK_UINT(blobmsg_get_u32(channel_fields[CHANNEL_OPTIONS_INTERVAL]), 0);
	if (channel_fields[CHANNEL_G722_PTYPE])
		CHECK_UINT(blobmsg_get_u32(channel_fields[CHANNEL_G722_PTYPE]), 9);
	if (channel_fields[CHANNEL_OPUS_PTYPE])
		CHECK_UINT(blobmsg_get_u32(channel_fields[CHANNEL_OPUS_PTYPE]), 96);
	if (channel_fields[CHANNEL_TIMERS])
		blobmsg_parse_attr(channel_timer_policy, TIMER_MAX, timer_fields,
				   channel_fields[CHANNEL_TIMERS]);
	for (i = 0; i < TIMER_MAX; i++) {
		CHECK(timer_fields[i] != NULL);
		if (timer_fields[i])
			CHECK_UINT(blobmsg_get_u32(timer_fields[i]), default_timers[i]);
	}
	blob_buf_free(&buf);
	fixture_free(&fixture);

	CASE("the channel dump never carries sip_secret");
	memset(channel_fields, 0, sizeof(channel_fields));
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(
		"\toption sip_secret 'private-value'\n"));
	if (!channel_dump_fields_get(fixture.cfg, &buf, channel_fields)) {
		blob_buf_free(&buf);
		fixture_free(&fixture);
		return;
	}
	CHECK(channel_fields[CHANNEL_SIP_USERNAME] != NULL);
	CHECK(channel_fields[CHANNEL_SIP_SECRET] == NULL);
	blob_buf_free(&buf);
	fixture_free(&fixture);
}

/*
 * The diagnostic dump. tls_cert_username and tls_cert_password are the
 * download credentials of cfg.h. That header keeps both out of the dump, as
 * sip_secret stays out of the channel dump.
 */
static void tls_dump_tests(void)
{
	enum {
		DUMP_TLS_CERT_PATH,
		DUMP_TLS_CERT_USERNAME,
		DUMP_TLS_CERT_PASSWORD,
		DUMP_TLS_VERIFY,
		DUMP_TLS_KEYLOG,
		DUMP_CHANNEL,
		DUMP_MAX,
	};
	enum {
		CHANNEL_SIP_PORT,
		CHANNEL_SIP_PORT_SET,
		CHANNEL_MAX,
	};
	static const struct blobmsg_policy dump_policy[DUMP_MAX] = {
		[DUMP_TLS_CERT_PATH] = {
			.name = "tls_cert_path",
			.type = BLOBMSG_TYPE_STRING,
		},
		[DUMP_TLS_CERT_USERNAME] = {
			.name = "tls_cert_username",
			.type = BLOBMSG_TYPE_STRING,
		},
		[DUMP_TLS_CERT_PASSWORD] = {
			.name = "tls_cert_password",
			.type = BLOBMSG_TYPE_STRING,
		},
		[DUMP_TLS_VERIFY] = {
			.name = "tls_server_verification_enabled",
			.type = BLOBMSG_TYPE_BOOL,
		},
		[DUMP_TLS_KEYLOG] = {
			.name = "tls_keylog_enabled",
			.type = BLOBMSG_TYPE_BOOL,
		},
		[DUMP_CHANNEL] = { .name = "channel", .type = BLOBMSG_TYPE_ARRAY },
	};
	static const struct blobmsg_policy channel_policy[CHANNEL_MAX] = {
		[CHANNEL_SIP_PORT] = {
			.name = "sip_port",
			.type = BLOBMSG_TYPE_INT32,
		},
		[CHANNEL_SIP_PORT_SET] = {
			.name = "sip_port_set",
			.type = BLOBMSG_TYPE_BOOL,
		},
	};
	struct blob_attr *dump_fields[DUMP_MAX] = { 0 };
	struct blob_attr *channel_fields[CHANNEL_MAX] = { 0 };
	struct blob_attr *channel_entry;
	struct cfg_fixture fixture;
	struct blob_buf buf = { 0 };

	CASE("the dump carries the TLS fields but never the credentials");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(tls_config));
	blobmsg_buf_init(&buf);
	CHECK(buf.head != NULL);
	if (!buf.head) {
		blob_buf_free(&buf);
		fixture_free(&fixture);
		return;
	}

	cfg_dump(fixture.cfg, &buf);
	blobmsg_parse_attr(dump_policy, DUMP_MAX, dump_fields, buf.head);
	CHECK(dump_fields[DUMP_TLS_CERT_PATH] != NULL);
	CHECK(dump_fields[DUMP_TLS_CERT_USERNAME] == NULL);
	CHECK(dump_fields[DUMP_TLS_CERT_PASSWORD] == NULL);
	CHECK(dump_fields[DUMP_TLS_VERIFY] != NULL);
	CHECK(dump_fields[DUMP_TLS_KEYLOG] != NULL);
	if (dump_fields[DUMP_TLS_CERT_PATH])
		CHECK_CSTR(blobmsg_get_string(dump_fields[DUMP_TLS_CERT_PATH]),
			   "https://certs.example.test/bundle.tgz");
	if (dump_fields[DUMP_TLS_VERIFY])
		CHECK(blobmsg_get_bool(dump_fields[DUMP_TLS_VERIFY]));
	if (dump_fields[DUMP_TLS_KEYLOG])
		CHECK(blobmsg_get_bool(dump_fields[DUMP_TLS_KEYLOG]));

	CASE("the dump reports the sip_port presence flag");
	channel_entry = dump_fields[DUMP_CHANNEL] ?
		(struct blob_attr *)blobmsg_data(dump_fields[DUMP_CHANNEL]) : NULL;
	if (channel_entry)
		blobmsg_parse_attr(channel_policy, CHANNEL_MAX, channel_fields,
				   channel_entry);
	CHECK(channel_fields[CHANNEL_SIP_PORT] != NULL);
	CHECK(channel_fields[CHANNEL_SIP_PORT_SET] != NULL);
	if (channel_fields[CHANNEL_SIP_PORT])
		CHECK_UINT(blobmsg_get_u32(channel_fields[CHANNEL_SIP_PORT]), 5080);
	if (channel_fields[CHANNEL_SIP_PORT_SET])
		CHECK(blobmsg_get_bool(channel_fields[CHANNEL_SIP_PORT_SET]));

	blob_buf_free(&buf);
	fixture_free(&fixture);

	CASE("an absent sip_port is reported as absent in the dump");
	memset(dump_fields, 0, sizeof(dump_fields));
	memset(channel_fields, 0, sizeof(channel_fields));
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(""));
	blobmsg_buf_init(&buf);
	CHECK(buf.head != NULL);
	if (!buf.head) {
		blob_buf_free(&buf);
		fixture_free(&fixture);
		return;
	}

	cfg_dump(fixture.cfg, &buf);
	blobmsg_parse_attr(dump_policy, DUMP_MAX, dump_fields, buf.head);
	channel_entry = dump_fields[DUMP_CHANNEL] ?
		(struct blob_attr *)blobmsg_data(dump_fields[DUMP_CHANNEL]) : NULL;
	if (channel_entry)
		blobmsg_parse_attr(channel_policy, CHANNEL_MAX, channel_fields,
				   channel_entry);
	CHECK(channel_fields[CHANNEL_SIP_PORT_SET] != NULL);
	if (channel_fields[CHANNEL_SIP_PORT_SET])
		CHECK(!blobmsg_get_bool(channel_fields[CHANNEL_SIP_PORT_SET]));
	if (channel_fields[CHANNEL_SIP_PORT])
		CHECK_UINT(blobmsg_get_u32(channel_fields[CHANNEL_SIP_PORT]), 5060);

	blob_buf_free(&buf);
	fixture_free(&fixture);
}

static void dump_tests(void)
{
	enum {
		DUMP_LOADED,
		DUMP_CHANNELS,
		DUMP_TRANSPORT,
		DUMP_CHANNEL,
		DUMP_CODEC,
		DUMP_MAX,
	};
	enum {
		CHANNEL_FLASH_RELAY,
		CHANNEL_CALL_FWD_LOCAL,
		CHANNEL_DISTINCTIVE_RING_9,
		CHANNEL_AMWI,
		CHANNEL_VMWI,
		CHANNEL_FWD_DISC_DELAY,
		CHANNEL_FWD_DISC_BATTERY,
		CHANNEL_MAX,
	};
	enum {
		CODEC_PRIORITY,
		CODEC_DEFAULT_PRIORITY,
		CODEC_BITRATE,
		CODEC_FEC,
		CODEC_COMPLEXITY,
		CODEC_MAX,
	};
	static const struct blobmsg_policy dump_policy[DUMP_MAX] = {
		[DUMP_LOADED] = { .name = "loaded", .type = BLOBMSG_TYPE_BOOL },
		[DUMP_CHANNELS] = { .name = "channels", .type = BLOBMSG_TYPE_INT32 },
		[DUMP_TRANSPORT] = { .name = "transport", .type = BLOBMSG_TYPE_STRING },
		[DUMP_CHANNEL] = { .name = "channel", .type = BLOBMSG_TYPE_ARRAY },
		[DUMP_CODEC] = { .name = "codec", .type = BLOBMSG_TYPE_ARRAY },
	};
	static const struct blobmsg_policy channel_policy[CHANNEL_MAX] = {
		[CHANNEL_FLASH_RELAY] = {
			.name = "sip_flash_relay",
			.type = BLOBMSG_TYPE_STRING,
		},
		[CHANNEL_CALL_FWD_LOCAL] = {
			.name = "call_fwd_local",
			.type = BLOBMSG_TYPE_BOOL,
		},
		[CHANNEL_DISTINCTIVE_RING_9] = {
			.name = "distinctive_ring_9",
			.type = BLOBMSG_TYPE_STRING,
		},
		[CHANNEL_AMWI] = { .name = "amwi", .type = BLOBMSG_TYPE_BOOL },
		[CHANNEL_VMWI] = { .name = "vmwi", .type = BLOBMSG_TYPE_BOOL },
		[CHANNEL_FWD_DISC_DELAY] = {
			.name = "forward_disconnect_delay",
			.type = BLOBMSG_TYPE_INT32,
		},
		[CHANNEL_FWD_DISC_BATTERY] = {
			.name = "forward_disconnect_battery",
			.type = BLOBMSG_TYPE_STRING,
		},
	};
	static const struct blobmsg_policy codec_policy[CODEC_MAX] = {
		[CODEC_PRIORITY] = { .name = "priority", .type = BLOBMSG_TYPE_INT32 },
		[CODEC_DEFAULT_PRIORITY] = {
			.name = "default_priority",
			.type = BLOBMSG_TYPE_INT32,
		},
		[CODEC_BITRATE] = { .name = "bitrate", .type = BLOBMSG_TYPE_INT32 },
		[CODEC_FEC] = { .name = "fec", .type = BLOBMSG_TYPE_BOOL },
		[CODEC_COMPLEXITY] = {
			.name = "complexity",
			.type = BLOBMSG_TYPE_INT32,
		},
	};
	struct blob_attr *dump_fields[DUMP_MAX] = { 0 };
	struct blob_attr *channel_fields[CHANNEL_MAX] = { 0 };
	struct blob_attr *codec_fields[CODEC_MAX] = { 0 };
	struct blob_attr *channel_entry;
	struct blob_attr *codec_entry;
	struct cfg_fixture fixture;
	struct blob_buf buf = { 0 };

	CASE("cfg dump exposes base and codec fields");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(full_config));
	blobmsg_buf_init(&buf);
	CHECK(buf.head != NULL);
	if (!buf.head) {
		blob_buf_free(&buf);
		fixture_free(&fixture);
		return;
	}

	cfg_dump(fixture.cfg, &buf);
	blobmsg_parse_attr(dump_policy, DUMP_MAX, dump_fields, buf.head);
	CHECK(dump_fields[DUMP_LOADED] != NULL);
	CHECK(dump_fields[DUMP_CHANNELS] != NULL);
	CHECK(dump_fields[DUMP_TRANSPORT] != NULL);
	CHECK(dump_fields[DUMP_CHANNEL] != NULL);
	CHECK(dump_fields[DUMP_CODEC] != NULL);
	if (dump_fields[DUMP_LOADED])
		CHECK(blobmsg_get_bool(dump_fields[DUMP_LOADED]));
	if (dump_fields[DUMP_CHANNELS])
		CHECK_UINT(blobmsg_get_u32(dump_fields[DUMP_CHANNELS]), 1);
	if (dump_fields[DUMP_TRANSPORT])
		CHECK_CSTR(blobmsg_get_string(dump_fields[DUMP_TRANSPORT]), "udp");
	if (dump_fields[DUMP_CODEC])
		CHECK_UINT(blobmsg_check_array(dump_fields[DUMP_CODEC],
					       BLOBMSG_TYPE_TABLE), 1);

	channel_entry = dump_fields[DUMP_CHANNEL] ?
		(struct blob_attr *)blobmsg_data(dump_fields[DUMP_CHANNEL]) : NULL;
	if (channel_entry)
		blobmsg_parse_attr(channel_policy, CHANNEL_MAX, channel_fields,
				   channel_entry);
	CHECK(channel_fields[CHANNEL_FLASH_RELAY] != NULL);
	CHECK(channel_fields[CHANNEL_CALL_FWD_LOCAL] != NULL);
	CHECK(channel_fields[CHANNEL_DISTINCTIVE_RING_9] != NULL);
	if (channel_fields[CHANNEL_FLASH_RELAY])
		CHECK_CSTR(blobmsg_get_string(channel_fields[CHANNEL_FLASH_RELAY]),
			   "info");

	/* the two gates are dumped separately, never as one composite */
	CHECK(channel_fields[CHANNEL_AMWI] != NULL);
	CHECK(channel_fields[CHANNEL_VMWI] != NULL);
	if (channel_fields[CHANNEL_AMWI])
		CHECK(!blobmsg_get_bool(channel_fields[CHANNEL_AMWI]));
	if (channel_fields[CHANNEL_VMWI])
		CHECK(blobmsg_get_bool(channel_fields[CHANNEL_VMWI]));

	/* both halves of the forward disconnect pair reach the dump, because
	 * neither one alone says what the line will do */
	CHECK(channel_fields[CHANNEL_FWD_DISC_DELAY] != NULL);
	CHECK(channel_fields[CHANNEL_FWD_DISC_BATTERY] != NULL);
	if (channel_fields[CHANNEL_FWD_DISC_DELAY])
		CHECK_UINT(blobmsg_get_u32(channel_fields[CHANNEL_FWD_DISC_DELAY]), 900);
	if (channel_fields[CHANNEL_FWD_DISC_BATTERY])
		CHECK_CSTR(blobmsg_get_string(
			   channel_fields[CHANNEL_FWD_DISC_BATTERY]), "reverse");

	codec_entry = dump_fields[DUMP_CODEC] ?
		(struct blob_attr *)blobmsg_data(dump_fields[DUMP_CODEC]) : NULL;
	if (codec_entry)
		blobmsg_parse_attr(codec_policy, CODEC_MAX, codec_fields, codec_entry);
	CHECK(codec_fields[CODEC_PRIORITY] != NULL);
	CHECK(codec_fields[CODEC_DEFAULT_PRIORITY] != NULL);
	CHECK(codec_fields[CODEC_BITRATE] != NULL);
	CHECK(codec_fields[CODEC_FEC] != NULL);
	CHECK(codec_fields[CODEC_COMPLEXITY] != NULL);
	if (codec_fields[CODEC_PRIORITY])
		CHECK_UINT(blobmsg_get_u32(codec_fields[CODEC_PRIORITY]), 7);
	if (codec_fields[CODEC_DEFAULT_PRIORITY])
		CHECK_UINT(blobmsg_get_u32(codec_fields[CODEC_DEFAULT_PRIORITY]), 2);
	if (codec_fields[CODEC_BITRATE])
		CHECK_UINT(blobmsg_get_u32(codec_fields[CODEC_BITRATE]), 32000);
	if (codec_fields[CODEC_FEC])
		CHECK(!blobmsg_get_bool(codec_fields[CODEC_FEC]));
	if (codec_fields[CODEC_COMPLEXITY])
		CHECK_UINT(blobmsg_get_u32(codec_fields[CODEC_COMPLEXITY]), 7);

	blob_buf_free(&buf);
	fixture_free(&fixture);
}

/* the word a channel option dumps as, read back through the channel row */
static bool dump_word_get(const char *option, const char *field, char *out,
			  size_t out_len)
{
	enum { DUMP_KEY, DUMP_MAX };
	static const struct blobmsg_policy channel_policy[DUMP_MAX] = {
		[DUMP_KEY] = { .name = "channel", .type = BLOBMSG_TYPE_ARRAY },
	};
	struct blobmsg_policy field_policy[1] = {
		{ .name = field, .type = BLOBMSG_TYPE_STRING },
	};
	struct blob_attr *dump_fields[DUMP_MAX] = { 0 };
	struct blob_attr *field_fields[1] = { 0 };
	struct blob_attr *scope_entry;
	struct cfg_fixture fixture;
	struct blob_buf buf = { 0 };
	bool ok = false;

	fixture = channel_option_load(option);
	if (!fixture_ready(&fixture))
		return false;

	blobmsg_buf_init(&buf);
	if (!buf.head) {
		fixture_free(&fixture);
		return false;
	}

	cfg_dump(fixture.cfg, &buf);
	blobmsg_parse_attr(channel_policy, DUMP_MAX, dump_fields, buf.head);
	if (!dump_fields[DUMP_KEY])
		goto done;

	scope_entry = (struct blob_attr *)blobmsg_data(dump_fields[DUMP_KEY]);
	blobmsg_parse_attr(field_policy, 1, field_fields, scope_entry);
	if (!field_fields[0])
		goto done;

	snprintf(out, out_len, "%s", blobmsg_get_string(field_fields[0]));
	ok = true;

done:
	blob_buf_free(&buf);
	fixture_free(&fixture);
	return ok;
}

/* the word a base option dumps as, read at the top level of the dump */
static bool base_dump_word_get(const char *option, const char *field, char *out,
			       size_t out_len)
{
	struct blobmsg_policy field_policy[1] = {
		{ .name = field, .type = BLOBMSG_TYPE_STRING },
	};
	struct blob_attr *field_fields[1] = { 0 };
	struct cfg_fixture fixture;
	struct blob_buf buf = { 0 };
	char config[512];
	bool ok = false;

	snprintf(config, sizeof(config), "config base 'base'\n%s", option);
	fixture = fixture_load(config);
	if (!fixture_ready(&fixture))
		return false;

	blobmsg_buf_init(&buf);
	if (!buf.head) {
		fixture_free(&fixture);
		return false;
	}

	cfg_dump(fixture.cfg, &buf);
	blobmsg_parse_attr(field_policy, 1, field_fields, buf.head);
	if (field_fields[0]) {
		snprintf(out, out_len, "%s", blobmsg_get_string(field_fields[0]));
		ok = true;
	}

	blob_buf_free(&buf);
	fixture_free(&fixture);
	return ok;
}

static void dump_config_word_tests(void)
{
	static const struct word_example {
		const char *name;
		const char *option;
		const char *word;
	} dtmf_examples[] = {
		{ "sip_dtmf_mode dumps inband",
		  "\toption sip_dtmf_mode 'inband'\n", "inband" },
		{ "sip_dtmf_mode dumps rfc2833",
		  "\toption sip_dtmf_mode 'rfc2833'\n", "rfc2833" },
		{ "sip_dtmf_mode dumps info",
		  "\toption sip_dtmf_mode 'info'\n", "info" },
		{ "sip_dtmf_mode alias sipinfo dumps info",
		  "\toption sip_dtmf_mode 'sipinfo'\n", "info" },
		{ "sip_dtmf_mode alias sip_info dumps info",
		  "\toption sip_dtmf_mode 'sip_info'\n", "info" },
	}, flash_relay_examples[] = {
		{ "sip_flash_relay dumps off",
		  "\toption sip_flash_relay 'off'\n", "off" },
		{ "sip_flash_relay dumps rfc2833",
		  "\toption sip_flash_relay 'rfc2833'\n", "rfc2833" },
		{ "sip_flash_relay dumps info",
		  "\toption sip_flash_relay 'info'\n", "info" },
		{ "sip_flash_relay dumps info for a config that names it not",
		  "", "info" },
	}, refresher_examples[] = {
		{ "sip_session_refresher dumps uac",
		  "\toption sip_session_refresher 'uac'\n", "uac" },
		{ "sip_session_refresher dumps uas",
		  "\toption sip_session_refresher 'uas'\n", "uas" },
		{ "sip_session_refresher dumps unspecified",
		  "\toption sip_session_refresher 'unspecified'\n",
		  "unspecified" },
	}, battery_examples[] = {
		{ "forward_disconnect_battery dumps none",
		  "\toption forward_disconnect_battery 'none'\n", "none" },
		{ "forward_disconnect_battery dumps remove",
		  "\toption forward_disconnect_battery 'remove'\n", "remove" },
		{ "forward_disconnect_battery dumps reverse",
		  "\toption forward_disconnect_battery 'reverse'\n",
		  "reverse" },
	}, dns_srv_mode_examples[] = {
		{ "dns_srv_mode dumps auto",
		  "\toption dns_srv_mode 'auto'\n", "auto" },
		{ "dns_srv_mode dumps always",
		  "\toption dns_srv_mode 'always'\n", "always" },
		{ "dns_srv_mode dumps never",
		  "\toption dns_srv_mode 'never'\n", "never" },
		{ "dns_srv_mode dumps auto for a config that names it not",
		  "", "auto" },
	};
	enum {
		DUMP_CHANNEL,
		DUMP_RESTRICT_SOURCE,
		DUMP_MAX,
	};
	enum {
		CHANNEL_DIGIT_ESC_ENABLED,
		CHANNEL_LOCAL_CONF_ENABLED,
		CHANNEL_CALL_FWD_LOCAL_MODE,
		CHANNEL_MAX,
	};
	static const struct blobmsg_policy int_dump_policy[DUMP_MAX] = {
		[DUMP_CHANNEL] = { .name = "channel", .type = BLOBMSG_TYPE_ARRAY },
		[DUMP_RESTRICT_SOURCE] = {
			.name = "restrict_source",
			.type = BLOBMSG_TYPE_INT32,
		},
	};
	static const struct blobmsg_policy int_channel_policy[CHANNEL_MAX] = {
		[CHANNEL_DIGIT_ESC_ENABLED] = {
			.name = "digit_esc_enabled",
			.type = BLOBMSG_TYPE_INT32,
		},
		[CHANNEL_LOCAL_CONF_ENABLED] = {
			.name = "local_conf_enabled",
			.type = BLOBMSG_TYPE_INT32,
		},
		[CHANNEL_CALL_FWD_LOCAL_MODE] = {
			.name = "call_fwd_local_mode",
			.type = BLOBMSG_TYPE_INT32,
		},
	};
	struct blob_attr *int_dump_fields[DUMP_MAX] = { 0 };
	struct blob_attr *int_channel_fields[CHANNEL_MAX] = { 0 };
	struct blob_attr *channel_entry;
	struct cfg_fixture fixture;
	struct blob_buf buf = { 0 };
	char got[32];
	bool ok;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(dtmf_examples); i++) {
		CASE(dtmf_examples[i].name);
		ok = dump_word_get(dtmf_examples[i].option, "sip_dtmf_mode", got,
				   sizeof(got));
		CHECK(ok);
		if (ok)
			CHECK_CSTR(got, dtmf_examples[i].word);
	}

	for (i = 0; i < ARRAY_SIZE(flash_relay_examples); i++) {
		CASE(flash_relay_examples[i].name);
		ok = dump_word_get(flash_relay_examples[i].option,
				   "sip_flash_relay", got, sizeof(got));
		CHECK(ok);
		if (ok)
			CHECK_CSTR(got, flash_relay_examples[i].word);
	}

	for (i = 0; i < ARRAY_SIZE(refresher_examples); i++) {
		CASE(refresher_examples[i].name);
		ok = dump_word_get(refresher_examples[i].option,
				   "sip_session_refresher", got, sizeof(got));
		CHECK(ok);
		if (ok)
			CHECK_CSTR(got, refresher_examples[i].word);
	}

	for (i = 0; i < ARRAY_SIZE(battery_examples); i++) {
		CASE(battery_examples[i].name);
		ok = dump_word_get(battery_examples[i].option,
				   "forward_disconnect_battery", got,
				   sizeof(got));
		CHECK(ok);
		if (ok)
			CHECK_CSTR(got, battery_examples[i].word);
	}

	/* the word order of the table is pinned here; the static assert in
	 * uci.c pins its length alone */
	for (i = 0; i < ARRAY_SIZE(dns_srv_mode_examples); i++) {
		CASE(dns_srv_mode_examples[i].name);
		ok = base_dump_word_get(dns_srv_mode_examples[i].option,
					"dns_srv_mode", got, sizeof(got));
		CHECK(ok);
		if (ok)
			CHECK_CSTR(got, dns_srv_mode_examples[i].word);
	}

	CASE("the number-valued enums still dump as integers");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n\toption restrict_source '2'\n"
		"config channel 'channel1'\n\toption id '0'\n"
		"\toption digit_esc_enabled '2'\n"
		"\toption local_conf_enabled '1'\n"
		"\toption call_fwd_local_mode '4'\n"));
	blobmsg_buf_init(&buf);
	CHECK(buf.head != NULL);
	if (!buf.head) {
		fixture_free(&fixture);
		return;
	}

	cfg_dump(fixture.cfg, &buf);
	blobmsg_parse_attr(int_dump_policy, DUMP_MAX, int_dump_fields, buf.head);
	CHECK(int_dump_fields[DUMP_RESTRICT_SOURCE] != NULL);
	if (int_dump_fields[DUMP_RESTRICT_SOURCE])
		CHECK_UINT(blobmsg_get_u32(int_dump_fields[DUMP_RESTRICT_SOURCE]), 2);
	channel_entry = int_dump_fields[DUMP_CHANNEL] ?
		(struct blob_attr *)blobmsg_data(int_dump_fields[DUMP_CHANNEL]) :
		NULL;
	if (channel_entry)
		blobmsg_parse_attr(int_channel_policy, CHANNEL_MAX,
				   int_channel_fields, channel_entry);
	CHECK(int_channel_fields[CHANNEL_DIGIT_ESC_ENABLED] != NULL);
	CHECK(int_channel_fields[CHANNEL_LOCAL_CONF_ENABLED] != NULL);
	CHECK(int_channel_fields[CHANNEL_CALL_FWD_LOCAL_MODE] != NULL);
	if (int_channel_fields[CHANNEL_DIGIT_ESC_ENABLED])
		CHECK_UINT(blobmsg_get_u32(
			   int_channel_fields[CHANNEL_DIGIT_ESC_ENABLED]), 2);
	if (int_channel_fields[CHANNEL_LOCAL_CONF_ENABLED])
		CHECK_UINT(blobmsg_get_u32(
			   int_channel_fields[CHANNEL_LOCAL_CONF_ENABLED]), 1);
	if (int_channel_fields[CHANNEL_CALL_FWD_LOCAL_MODE])
		CHECK_UINT(blobmsg_get_u32(
			   int_channel_fields[CHANNEL_CALL_FWD_LOCAL_MODE]), 4);
	blob_buf_free(&buf);
	fixture_free(&fixture);
}

static void hal_schema_test(void)
{
	const struct cfg_option_desc *d;
	unsigned int count = 0;
	unsigned int i;

	CASE("schema: every HAL option is supported and restarts all");
	for (i = 0; cfg_hal_options[i].name; i++) {
		d = &cfg_hal_options[i];
		CHECK_UINT(d->support, SUP_SUPPORTED);
		CHECK_UINT(d->apply_class, APPLY_RESTART_ALL);
		CHECK(d->alias_of == NULL);
		count++;
	}
	CHECK_UINT(count, 15);
}

/*
 * The loader stores six options as a word index and takes their words from
 * their schema rows. Each such row must exist and carry a word list. Field
 * tables are static in uci.c, so this case names those rows.
 */
static void enum_index_schema_test(void)
{
	static const struct {
		const struct cfg_option_desc *tbl;
		const char *name;
	} rows[] = {
		{ cfg_base_options, "dns_srv_mode" },
		{ cfg_channel_options, "sip_flash_relay" },
		{ cfg_channel_options, "sip_session_refresher" },
		{ cfg_channel_options, "forward_disconnect_battery" },
		{ cfg_mgcp_options, "quarantine_default" },
		{ cfg_mgcp_options, "reply_method" },
	};
	const struct cfg_option_desc *d;
	unsigned int i;

	CASE("schema: each option stored as a word index has its word list");
	for (i = 0; i < ARRAY_SIZE(rows); i++) {
		for (d = rows[i].tbl; d->name; d++) {
			if (!strcmp(d->name, rows[i].name))
				break;
		}
		CHECK(d->name != NULL);
		CHECK_UINT(d->type, CFG_ENUM);
		CHECK(d->enum_vals != NULL);
	}
}

/*
 * The debug section. One row, and it is live: a reload is the whole apply,
 * because trace.c rewrites its uci mask from the string and nothing restarts.
 * A row that ever claimed another class would promise an operator a restart
 * that never comes.
 */
static void debug_schema_test(void)
{
	const struct cfg_option_desc *d;
	unsigned int count = 0;
	unsigned int i;

	CASE("schema: every debug option is live and supported");
	for (i = 0; cfg_debug_options[i].name; i++) {
		d = &cfg_debug_options[i];
		CHECK_UINT(d->support, SUP_SUPPORTED);
		CHECK_UINT(d->apply_class, APPLY_LIVE);
		CHECK(d->alias_of == NULL);
		count++;
	}
	CHECK_UINT(count, 1);
}

static void debug_tests(void)
{
	enum { DUMP_DEBUG, DUMP_MAX };
	enum { DBG_TRACE, DBG_MAX };
	static const struct blobmsg_policy dump_policy[DUMP_MAX] = {
		[DUMP_DEBUG] = { .name = "debug", .type = BLOBMSG_TYPE_TABLE },
	};
	static const struct blobmsg_policy debug_policy[DBG_MAX] = {
		[DBG_TRACE] = { .name = "trace", .type = BLOBMSG_TYPE_STRING },
	};
	struct blob_attr *dump_fields[DUMP_MAX] = { 0 };
	struct blob_attr *debug_fields[DBG_MAX] = { 0 };
	struct cfg_fixture fixture;
	struct blob_buf buf = { 0 };

	CASE("a config without a debug section names no trace domain");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load("config base 'base'\n"));
	CHECK_CSTR(fixture.cfg->debug.trace, "");
	fixture_free(&fixture);

	CASE("a debug section loads its trace list verbatim");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n"
		"config debug 'debug'\n\toption trace 'media,jb'\n"));
	CHECK_CSTR(fixture.cfg->debug.trace, "media,jb");
	fixture_free(&fixture);

	CASE("an anonymous debug section loads as the first debug section");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config debug\n\toption trace 'jb'\n"));
	CHECK_CSTR(fixture.cfg->debug.trace, "jb");
	fixture_free(&fixture);

	CASE("the first debug section wins and each additional section warns");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config debug 'first'\n\toption trace 'media'\n"
		"config debug 'debug'\n\toption trace 'sip'\n"
		"config debug\n\toption trace 'all'\n"));
	CHECK_CSTR(fixture.cfg->debug.trace, "media");
	CHECK_UINT(warn_capture_count("additional debug section ignored"), 2);
	warn_capture_end();
	fixture_free(&fixture);

	CASE("the dump carries the loaded trace list");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config debug 'debug'\n\toption trace 'media,jb'\n"));
	blobmsg_buf_init(&buf);
	CHECK(buf.head != NULL);
	if (!buf.head) {
		blob_buf_free(&buf);
		fixture_free(&fixture);
		return;
	}

	cfg_dump(fixture.cfg, &buf);
	blobmsg_parse_attr(dump_policy, DUMP_MAX, dump_fields, buf.head);
	CHECK(dump_fields[DUMP_DEBUG] != NULL);
	if (dump_fields[DUMP_DEBUG])
		blobmsg_parse_attr(debug_policy, DBG_MAX, debug_fields,
				   dump_fields[DUMP_DEBUG]);
	CHECK(debug_fields[DBG_TRACE] != NULL);
	if (debug_fields[DBG_TRACE])
		CHECK_CSTR(blobmsg_get_string(debug_fields[DBG_TRACE]),
			   "media,jb");
	blob_buf_free(&buf);
	fixture_free(&fixture);

	CASE("the dump reports a defaulted trace list as empty");
	memset(dump_fields, 0, sizeof(dump_fields));
	memset(debug_fields, 0, sizeof(debug_fields));
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load("config base 'base'\n"));
	blobmsg_buf_init(&buf);
	CHECK(buf.head != NULL);
	if (!buf.head) {
		blob_buf_free(&buf);
		fixture_free(&fixture);
		return;
	}

	cfg_dump(fixture.cfg, &buf);
	blobmsg_parse_attr(dump_policy, DUMP_MAX, dump_fields, buf.head);
	CHECK(dump_fields[DUMP_DEBUG] != NULL);
	if (dump_fields[DUMP_DEBUG])
		blobmsg_parse_attr(debug_policy, DBG_MAX, debug_fields,
				   dump_fields[DUMP_DEBUG]);
	CHECK(debug_fields[DBG_TRACE] != NULL);
	if (debug_fields[DBG_TRACE])
		CHECK_CSTR(blobmsg_get_string(debug_fields[DBG_TRACE]), "");
	blob_buf_free(&buf);
	fixture_free(&fixture);
}

static void hal_default_tests(void)
{
	struct cfg_fixture fixture;
	const struct cfg_hal *hal;

	CASE("a config without a HAL section takes every board default");
	setenv("PDB_NUM_POTS", "4", 1);
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load("config base 'base'\n"));
	hal = &fixture.cfg->hal;
	CHECK_CSTR(hal->tdm_backend, "alsa-raw");
	CHECK_CSTR(hal->tdm_playback, "plughw:0,0");
	CHECK_CSTR(hal->tdm_capture, "plughw:0,1");
	CHECK_CSTR(hal->slic_plugin, "/usr/lib/udsp/udsp-slic-proslic-mt798x.so");
	CHECK_CSTR(hal->fax_plugin, "");
	CHECK_CSTR(hal->spidev, "/dev/spidev0.0");
	CHECK_CSTR(hal->gpio_reset, "slic_reset");
	CHECK_CSTR(hal->gpio_interrupt, "slic_interrupt");
	CHECK_UINT(hal->populated_ports_cfg, 0);
	CHECK_UINT(hal->populated_ports, 4);
	CHECK_UINT(hal->channels, 2);
	CHECK_UINT(hal->prefill_periods, 3);
	CHECK_UINT(hal->engine_cpu, 3);
	CHECK_UINT(hal->engine_prio, 50);
	CHECK_UINT(hal->pcm_offset_bits, 0);
	CHECK_UINT(hal->sample_rate, 8000);
	fixture_free(&fixture);
	unsetenv("PDB_NUM_POTS");

	CASE("a missing voip package takes the HAL board defaults");
	setenv("PDB_NUM_POTS", "5", 1);
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(NULL));
	CHECK(!fixture.cfg->loaded);
	CHECK_CSTR(fixture.cfg->hal.tdm_playback, "plughw:0,0");
	CHECK_CSTR(fixture.cfg->hal.slic_plugin,
		   "/usr/lib/udsp/udsp-slic-proslic-mt798x.so");
	CHECK_UINT(fixture.cfg->hal.populated_ports_cfg, 0);
	CHECK_UINT(fixture.cfg->hal.populated_ports, 5);
	CHECK_UINT(fixture.cfg->hal.channels, 2);
	CHECK_UINT(fixture.cfg->hal.prefill_periods, 3);
	CHECK_UINT(fixture.cfg->hal.engine_cpu, 3);
	CHECK_UINT(fixture.cfg->hal.engine_prio, 50);
	CHECK_UINT(fixture.cfg->hal.pcm_offset_bits, 0);
	CHECK_UINT(fixture.cfg->hal.sample_rate, 8000);
	fixture_free(&fixture);
	unsetenv("PDB_NUM_POTS");
}

static void hal_explicit_tests(void)
{
	static const char config[] =
		"\toption tdm_backend 'file'\n"
		"\toption tdm_playback '/tmp/playback.raw'\n"
		"\toption tdm_capture '/tmp/capture.raw'\n"
		"\toption slic_plugin '/tmp/slic.so'\n"
		"\toption fax_plugin '/tmp/fax.so'\n"
		"\toption spidev '/dev/spidev1.2'\n"
		"\toption gpio_reset 'reset-x'\n"
		"\toption gpio_interrupt 'irq-x'\n"
		"\toption populated_ports '6'\n"
		"\toption channels '1'\n"
		"\toption prefill_periods '9'\n"
		"\toption engine_cpu '-1'\n"
		"\toption engine_prio '0'\n"
		"\toption pcm_offset_bits '7'\n"
		"\toption sample_rate '16000'\n";
	struct cfg_fixture fixture;
	const struct cfg_hal *hal;

	CASE("every explicit HAL option loads from the named section");
	setenv("PDB_NUM_POTS", "8", 1);
	FIXTURE_LOAD_OR_RETURN(fixture, hal_option_load(config));
	hal = &fixture.cfg->hal;
	CHECK_CSTR(hal->tdm_backend, "file");
	CHECK_CSTR(hal->tdm_playback, "/tmp/playback.raw");
	CHECK_CSTR(hal->tdm_capture, "/tmp/capture.raw");
	CHECK_CSTR(hal->slic_plugin, "/tmp/slic.so");
	CHECK_CSTR(hal->fax_plugin, "/tmp/fax.so");
	CHECK_CSTR(hal->spidev, "/dev/spidev1.2");
	CHECK_CSTR(hal->gpio_reset, "reset-x");
	CHECK_CSTR(hal->gpio_interrupt, "irq-x");
	CHECK_UINT(hal->populated_ports_cfg, 6);
	CHECK_UINT(hal->populated_ports, 6);
	CHECK_UINT(hal->channels, 1);
	CHECK_UINT(hal->prefill_periods, 9);
	CHECK_UINT(hal->engine_cpu, -1);
	CHECK_UINT(hal->engine_prio, 0);
	CHECK_UINT(hal->pcm_offset_bits, 7);
	CHECK_UINT(hal->sample_rate, 16000);
	fixture_free(&fixture);
	unsetenv("PDB_NUM_POTS");

	CASE("alsa-raw HAL backend loads from the named section");
	FIXTURE_LOAD_OR_RETURN(fixture,
		hal_option_load("\toption tdm_backend 'alsa-raw'\n"));
	CHECK_CSTR(fixture.cfg->hal.tdm_backend, "alsa-raw");
	fixture_free(&fixture);

	CASE("an anonymous HAL section loads as the first HAL section");
	setenv("PDB_NUM_POTS", "2", 1);
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config hal\n\toption channels '1'\n"));
	CHECK_UINT(fixture.cfg->hal.channels, 1);
	fixture_free(&fixture);

	CASE("the first HAL section wins and each additional section warns");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config hal 'first'\n\toption channels '1'\n"
		"config hal 'hal'\n\toption channels '2'\n"
		"config hal\n\toption channels '2'\n"));
	CHECK_UINT(fixture.cfg->hal.channels, 1);
	CHECK_UINT(warn_capture_count("additional hal section ignored"), 2);
	warn_capture_end();
	fixture_free(&fixture);
	unsetenv("PDB_NUM_POTS");
}

static void hal_validation_tests(void)
{
	static const char long_value[] =
		"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
		"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
		"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
	struct cfg_fixture fixture;
	char config[2048];

	CASE("an invalid HAL backend takes the enum default");
	setenv("PDB_NUM_POTS", "2", 1);
	FIXTURE_LOAD_OR_RETURN(fixture,
		hal_option_load("\toption tdm_backend 'socket'\n"));
	CHECK_CSTR(fixture.cfg->hal.tdm_backend, "alsa-raw");
	fixture_free(&fixture);

	CASE("gpio_interrupt none selects polling");
	FIXTURE_LOAD_OR_RETURN(fixture,
		hal_option_load("\toption gpio_interrupt 'none'\n"));
	CHECK_CSTR(fixture.cfg->hal.gpio_interrupt, "");
	fixture_free(&fixture);

	CASE("slic_plugin none selects the fake backend");
	FIXTURE_LOAD_OR_RETURN(fixture,
		hal_option_load("\toption slic_plugin 'none'\n"));
	CHECK_CSTR(fixture.cfg->hal.slic_plugin, "");
	fixture_free(&fixture);

	CASE("empty HAL sentinel options keep their schema defaults");
	FIXTURE_LOAD_OR_RETURN(fixture, hal_option_load(
		"\toption gpio_interrupt ''\n\toption slic_plugin ''\n"));
	CHECK_CSTR(fixture.cfg->hal.gpio_interrupt, "slic_interrupt");
	CHECK_CSTR(fixture.cfg->hal.slic_plugin,
		   "/usr/lib/udsp/udsp-slic-proslic-mt798x.so");
	fixture_free(&fixture);

	CASE("a list-form gpio_interrupt keeps its schema default");
	FIXTURE_LOAD_OR_RETURN(fixture,
		hal_option_load("\tlist gpio_interrupt 'none'\n"));
	CHECK_CSTR(fixture.cfg->hal.gpio_interrupt, "slic_interrupt");
	fixture_free(&fixture);

	CASE("HAL strings truncate to their destination bounds");
	snprintf(config, sizeof(config),
		 "config hal 'hal'\n"
		 "\toption tdm_playback '%s'\n\toption tdm_capture '%s'\n"
		 "\toption slic_plugin '%s'\n\toption fax_plugin '%s'\n"
		 "\toption spidev '%s'\n\toption gpio_reset '%s'\n"
		 "\toption gpio_interrupt '%s'\n",
		 long_value, long_value, long_value, long_value, long_value,
		 long_value, long_value);
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(config));
	CHECK_UINT(strlen(fixture.cfg->hal.tdm_playback), 63);
	CHECK_UINT(strlen(fixture.cfg->hal.tdm_capture), 63);
	CHECK_UINT(strlen(fixture.cfg->hal.slic_plugin), 127);
	CHECK_UINT(strlen(fixture.cfg->hal.fax_plugin), 127);
	CHECK_UINT(strlen(fixture.cfg->hal.spidev), 63);
	CHECK_UINT(strlen(fixture.cfg->hal.gpio_reset), 31);
	CHECK_UINT(strlen(fixture.cfg->hal.gpio_interrupt), 31);
	fixture_free(&fixture);

	CASE("HAL integers below their bounds clamp or take the environment");
	setenv("PDB_NUM_POTS", "3", 1);
	FIXTURE_LOAD_OR_RETURN(fixture, hal_option_load(
		"\toption populated_ports '-1'\n\toption channels '0'\n"
		"\toption prefill_periods '0'\n\toption engine_cpu '-2'\n"
		"\toption engine_prio '-1'\n"));
	CHECK_UINT(fixture.cfg->hal.populated_ports_cfg, 0);
	CHECK_UINT(fixture.cfg->hal.populated_ports, 3);
	CHECK_UINT(fixture.cfg->hal.channels, 1);
	CHECK_UINT(fixture.cfg->hal.prefill_periods, 1);
	CHECK_UINT(fixture.cfg->hal.engine_cpu, -1);
	CHECK_UINT(fixture.cfg->hal.engine_prio, 0);
	fixture_free(&fixture);

	CASE("HAL integers above their bounds clamp to each maximum");
	FIXTURE_LOAD_OR_RETURN(fixture, hal_option_load(
		"\toption populated_ports '9'\n\toption channels '3'\n"
		"\toption prefill_periods '17'\n\toption engine_cpu '256'\n"
		"\toption engine_prio '100'\n"
		"\toption pcm_offset_bits '16'\n"));
	CHECK_UINT(fixture.cfg->hal.populated_ports_cfg, 8);
	CHECK_UINT(fixture.cfg->hal.populated_ports, 8);
	CHECK_UINT(fixture.cfg->hal.channels, 2);
	CHECK_UINT(fixture.cfg->hal.prefill_periods, 16);
	CHECK_UINT(fixture.cfg->hal.engine_cpu, 255);
	CHECK_UINT(fixture.cfg->hal.engine_prio, 99);
	CHECK_UINT(fixture.cfg->hal.pcm_offset_bits, 15);
	fixture_free(&fixture);

	CASE("sample_rate accepts only the two line rates");
	FIXTURE_LOAD_OR_RETURN(fixture,
		hal_option_load("\toption sample_rate '12000'\n"));
	CHECK_UINT(fixture.cfg->hal.sample_rate, 8000);
	fixture_free(&fixture);
	FIXTURE_LOAD_OR_RETURN(fixture,
		hal_option_load("\toption sample_rate '16000'\n"));
	CHECK_UINT(fixture.cfg->hal.sample_rate, 16000);
	fixture_free(&fixture);

	CASE("malformed HAL integers take their schema defaults");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, hal_option_load(
		"\toption populated_ports 'bad'\n\toption channels 'bad'\n"
		"\toption prefill_periods 'bad'\n\toption engine_cpu 'bad'\n"
		"\toption engine_prio 'bad'\n"
		"\toption pcm_offset_bits 'bad'\n"));
	CHECK_UINT(fixture.cfg->hal.populated_ports_cfg, 0);
	CHECK_UINT(fixture.cfg->hal.populated_ports, 3);
	CHECK_UINT(fixture.cfg->hal.channels, 2);
	CHECK_UINT(fixture.cfg->hal.prefill_periods, 3);
	CHECK_UINT(fixture.cfg->hal.engine_cpu, 3);
	CHECK_UINT(fixture.cfg->hal.engine_prio, 50);
	CHECK_UINT(fixture.cfg->hal.pcm_offset_bits, 0);
	CHECK_UINT(warn_capture_count(
		"voip pcm_offset_bits value 'bad' is not a number"), 1);
	fixture_free(&fixture);
	warn_capture_end();

	CASE("a negative pcm_offset_bits takes zero and warns once");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture,
		hal_option_load("\toption pcm_offset_bits '-1'\n"));
	CHECK_UINT(fixture.cfg->hal.pcm_offset_bits, 0);
	CHECK_UINT(warn_capture_count(
		"voip pcm_offset_bits value '-1' is not a number"), 1);
	CHECK_UINT(warn_capture_count("is not a number"), 1);
	fixture_free(&fixture);
	warn_capture_end();
	unsetenv("PDB_NUM_POTS");
}

static void hal_environment_tests(void)
{
	struct cfg_fixture fixture;

	CASE("PDB_NUM_POTS one is the accepted lower edge");
	setenv("PDB_NUM_POTS", "1", 1);
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, hal_option_load(""));
	CHECK_UINT(fixture.cfg->hal.populated_ports, 1);
	CHECK_UINT(warn_capture_count("PDB_NUM_POTS"), 0);
	warn_capture_end();
	fixture_free(&fixture);

	CASE("PDB_NUM_POTS eight is the accepted upper edge");
	setenv("PDB_NUM_POTS", "8", 1);
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, hal_option_load(""));
	CHECK_UINT(fixture.cfg->hal.populated_ports, 8);
	CHECK_UINT(warn_capture_count("PDB_NUM_POTS"), 0);
	warn_capture_end();
	fixture_free(&fixture);

	CASE("populated_ports zero resolves from PDB_NUM_POTS");
	setenv("PDB_NUM_POTS", "7", 1);
	FIXTURE_LOAD_OR_RETURN(fixture,
		hal_option_load("\toption populated_ports '0'\n"));
	CHECK_UINT(fixture.cfg->hal.populated_ports_cfg, 0);
	CHECK_UINT(fixture.cfg->hal.populated_ports, 7);
	fixture_free(&fixture);

	CASE("an unset PDB_NUM_POTS falls back to one and warns");
	unsetenv("PDB_NUM_POTS");
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, hal_option_load(""));
	CHECK_UINT(fixture.cfg->hal.populated_ports, 1);
	CHECK_UINT(warn_capture_count("PDB_NUM_POTS is unset or invalid"), 1);
	warn_capture_end();
	fixture_free(&fixture);

	CASE("an invalid PDB_NUM_POTS falls back to one and warns");
	setenv("PDB_NUM_POTS", "9", 1);
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture, hal_option_load(""));
	CHECK_UINT(fixture.cfg->hal.populated_ports, 1);
	CHECK_UINT(warn_capture_count("PDB_NUM_POTS is unset or invalid"), 1);
	warn_capture_end();
	fixture_free(&fixture);

	CASE("a configured port count overrides PDB_NUM_POTS");
	setenv("PDB_NUM_POTS", "bad", 1);
	warn_capture_begin();
	FIXTURE_LOAD_OR_RETURN(fixture,
		hal_option_load("\toption populated_ports '5'\n"));
	CHECK_UINT(fixture.cfg->hal.populated_ports_cfg, 5);
	CHECK_UINT(fixture.cfg->hal.populated_ports, 5);
	CHECK_UINT(warn_capture_count("PDB_NUM_POTS"), 0);
	warn_capture_end();
	fixture_free(&fixture);
	unsetenv("PDB_NUM_POTS");
}

static void hal_dump_test(void)
{
	enum { DUMP_HAL, DUMP_MAX };
	enum {
		HAL_TDM_BACKEND, HAL_TDM_PLAYBACK, HAL_TDM_CAPTURE,
		HAL_SLIC_PLUGIN, HAL_FAX_PLUGIN, HAL_SPIDEV, HAL_GPIO_RESET,
		HAL_GPIO_INTERRUPT, HAL_POPULATED_PORTS_CFG, HAL_POPULATED_PORTS,
		HAL_CHANNELS, HAL_PREFILL_PERIODS, HAL_ENGINE_CPU, HAL_ENGINE_PRIO,
		HAL_PCM_OFFSET_BITS, HAL_SAMPLE_RATE,
		HAL_MAX,
	};
	static const struct blobmsg_policy dump_policy[DUMP_MAX] = {
		[DUMP_HAL] = { .name = "hal", .type = BLOBMSG_TYPE_TABLE },
	};
	static const struct blobmsg_policy hal_policy[HAL_MAX] = {
		[HAL_TDM_BACKEND] = {
			.name = "tdm_backend", .type = BLOBMSG_TYPE_STRING,
		},
		[HAL_TDM_PLAYBACK] = {
			.name = "tdm_playback", .type = BLOBMSG_TYPE_STRING,
		},
		[HAL_TDM_CAPTURE] = {
			.name = "tdm_capture", .type = BLOBMSG_TYPE_STRING,
		},
		[HAL_SLIC_PLUGIN] = {
			.name = "slic_plugin", .type = BLOBMSG_TYPE_STRING,
		},
		[HAL_FAX_PLUGIN] = {
			.name = "fax_plugin", .type = BLOBMSG_TYPE_STRING,
		},
		[HAL_SPIDEV] = {
			.name = "spidev", .type = BLOBMSG_TYPE_STRING,
		},
		[HAL_GPIO_RESET] = {
			.name = "gpio_reset", .type = BLOBMSG_TYPE_STRING,
		},
		[HAL_GPIO_INTERRUPT] = {
			.name = "gpio_interrupt", .type = BLOBMSG_TYPE_STRING,
		},
		[HAL_POPULATED_PORTS_CFG] = {
			.name = "populated_ports_cfg", .type = BLOBMSG_TYPE_INT32,
		},
		[HAL_POPULATED_PORTS] = {
			.name = "populated_ports", .type = BLOBMSG_TYPE_INT32,
		},
		[HAL_CHANNELS] = {
			.name = "channels", .type = BLOBMSG_TYPE_INT32,
		},
		[HAL_PREFILL_PERIODS] = {
			.name = "prefill_periods", .type = BLOBMSG_TYPE_INT32,
		},
		[HAL_ENGINE_CPU] = {
			.name = "engine_cpu", .type = BLOBMSG_TYPE_INT32,
		},
		[HAL_ENGINE_PRIO] = {
			.name = "engine_prio", .type = BLOBMSG_TYPE_INT32,
		},
		[HAL_PCM_OFFSET_BITS] = {
			.name = "pcm_offset_bits", .type = BLOBMSG_TYPE_INT32,
		},
		[HAL_SAMPLE_RATE] = {
			.name = "sample_rate", .type = BLOBMSG_TYPE_INT32,
		},
	};
	struct blob_attr *dump_fields[DUMP_MAX] = { 0 };
	struct blob_attr *hal_fields[HAL_MAX] = { 0 };
	struct cfg_fixture fixture;
	struct blob_buf buf = { 0 };

	CASE("the dump carries the loaded value of every HAL row");
	setenv("PDB_NUM_POTS", "7", 1);
	FIXTURE_LOAD_OR_RETURN(fixture, hal_option_load(
		"\toption tdm_backend 'file'\n"
		"\toption tdm_playback '/tmp/play.raw'\n"
		"\toption tdm_capture '/tmp/capture.raw'\n"
		"\toption slic_plugin '/tmp/slic.so'\n"
		"\toption fax_plugin '/tmp/fax.so'\n"
		"\toption spidev '/dev/spidev1.2'\n"
		"\toption gpio_reset 'reset-x'\n"
		"\toption gpio_interrupt 'irq-x'\n"
		"\toption populated_ports '0'\n"
		"\toption channels '1'\n"
		"\toption prefill_periods '9'\n"
		"\toption engine_cpu '-1'\n"
		"\toption engine_prio '0'\n"
		"\toption pcm_offset_bits '6'\n"
		"\toption sample_rate '16000'\n"));
	blobmsg_buf_init(&buf);
	CHECK(buf.head != NULL);
	if (!buf.head) {
		fixture_free(&fixture);
		unsetenv("PDB_NUM_POTS");
		return;
	}
	cfg_dump(fixture.cfg, &buf);
	blobmsg_parse_attr(dump_policy, DUMP_MAX, dump_fields, buf.head);
	CHECK(dump_fields[DUMP_HAL] != NULL);
	if (dump_fields[DUMP_HAL])
		blobmsg_parse_attr(hal_policy, HAL_MAX, hal_fields,
				   dump_fields[DUMP_HAL]);
	CHECK(hal_fields[HAL_TDM_BACKEND] != NULL);
	if (hal_fields[HAL_TDM_BACKEND])
		CHECK_CSTR(blobmsg_get_string(hal_fields[HAL_TDM_BACKEND]), "file");
	CHECK(hal_fields[HAL_TDM_PLAYBACK] != NULL);
	if (hal_fields[HAL_TDM_PLAYBACK])
		CHECK_CSTR(blobmsg_get_string(hal_fields[HAL_TDM_PLAYBACK]),
			   "/tmp/play.raw");
	CHECK(hal_fields[HAL_TDM_CAPTURE] != NULL);
	if (hal_fields[HAL_TDM_CAPTURE])
		CHECK_CSTR(blobmsg_get_string(hal_fields[HAL_TDM_CAPTURE]),
			   "/tmp/capture.raw");
	CHECK(hal_fields[HAL_SLIC_PLUGIN] != NULL);
	if (hal_fields[HAL_SLIC_PLUGIN])
		CHECK_CSTR(blobmsg_get_string(hal_fields[HAL_SLIC_PLUGIN]),
			   "/tmp/slic.so");
	CHECK(hal_fields[HAL_FAX_PLUGIN] != NULL);
	if (hal_fields[HAL_FAX_PLUGIN])
		CHECK_CSTR(blobmsg_get_string(hal_fields[HAL_FAX_PLUGIN]),
			   "/tmp/fax.so");
	CHECK(hal_fields[HAL_SPIDEV] != NULL);
	if (hal_fields[HAL_SPIDEV])
		CHECK_CSTR(blobmsg_get_string(hal_fields[HAL_SPIDEV]),
			   "/dev/spidev1.2");
	CHECK(hal_fields[HAL_GPIO_RESET] != NULL);
	if (hal_fields[HAL_GPIO_RESET])
		CHECK_CSTR(blobmsg_get_string(hal_fields[HAL_GPIO_RESET]), "reset-x");
	CHECK(hal_fields[HAL_GPIO_INTERRUPT] != NULL);
	if (hal_fields[HAL_GPIO_INTERRUPT])
		CHECK_CSTR(blobmsg_get_string(hal_fields[HAL_GPIO_INTERRUPT]),
			   "irq-x");
	CHECK(hal_fields[HAL_POPULATED_PORTS_CFG] != NULL);
	if (hal_fields[HAL_POPULATED_PORTS_CFG])
		CHECK_UINT(blobmsg_get_u32(hal_fields[HAL_POPULATED_PORTS_CFG]), 0);
	CHECK(hal_fields[HAL_POPULATED_PORTS] != NULL);
	if (hal_fields[HAL_POPULATED_PORTS])
		CHECK_UINT(blobmsg_get_u32(hal_fields[HAL_POPULATED_PORTS]), 7);
	CHECK(hal_fields[HAL_CHANNELS] != NULL);
	if (hal_fields[HAL_CHANNELS])
		CHECK_UINT(blobmsg_get_u32(hal_fields[HAL_CHANNELS]), 1);
	CHECK(hal_fields[HAL_PREFILL_PERIODS] != NULL);
	if (hal_fields[HAL_PREFILL_PERIODS])
		CHECK_UINT(blobmsg_get_u32(hal_fields[HAL_PREFILL_PERIODS]), 9);
	CHECK(hal_fields[HAL_ENGINE_CPU] != NULL);
	if (hal_fields[HAL_ENGINE_CPU])
		CHECK_UINT((int32_t)blobmsg_get_u32(hal_fields[HAL_ENGINE_CPU]), -1);
	CHECK(hal_fields[HAL_ENGINE_PRIO] != NULL);
	if (hal_fields[HAL_ENGINE_PRIO])
		CHECK_UINT((int32_t)blobmsg_get_u32(hal_fields[HAL_ENGINE_PRIO]), 0);
	CHECK(hal_fields[HAL_PCM_OFFSET_BITS] != NULL);
	if (hal_fields[HAL_PCM_OFFSET_BITS])
		CHECK_UINT(blobmsg_get_u32(hal_fields[HAL_PCM_OFFSET_BITS]), 6);
	CHECK(hal_fields[HAL_SAMPLE_RATE] != NULL);
	if (hal_fields[HAL_SAMPLE_RATE])
		CHECK_UINT(blobmsg_get_u32(hal_fields[HAL_SAMPLE_RATE]), 16000);
	blob_buf_free(&buf);
	fixture_free(&fixture);
	unsetenv("PDB_NUM_POTS");
}

/*
 * The support class and the apply class of fax_t38_enable, the T.38 gate of a
 * leg. A leg starts with T.38 enabled when this option and the backend
 * HAL_FEAT_T38 bit both hold. Other legs answer with the m=image port-0
 * refusal. Nothing in the daemon reads enum cfg_support, so only the
 * descriptor table tells an operator that the media path obeys this option.
 * A row that read accepted-unsupported would tell the operator the opposite.
 *
 * The leg reads the option once, at its own start, so a live call keeps its
 * gate and only a line restart changes it.
 */
static void fax_t38_support_test(void)
{
	const struct cfg_option_desc *d;
	unsigned int i;

	CASE("schema: fax_t38_enable is supported and restarts the line");
	for (i = 0; cfg_channel_options[i].name; i++) {
		d = &cfg_channel_options[i];
		if (strcmp(d->name, "fax_t38_enable"))
			continue;

		CHECK_UINT(d->support, SUP_SUPPORTED);
		CHECK_UINT(d->apply_class, APPLY_RESTART_LINE);
		return;
	}

	CHECK(!"fax_t38_enable is not in the channel option table");
}

static void g726_support_test(void)
{
	const struct cfg_option_desc *d;
	unsigned int i;

	CASE("The G.726 payload type is supported and restarts the line");
	for (i = 0; cfg_channel_options[i].name; i++) {
		d = &cfg_channel_options[i];
		if (strcmp(d->name, "g726_ptype"))
			continue;

		CHECK_UINT(d->support, SUP_SUPPORTED);
		CHECK_UINT(d->apply_class, APPLY_RESTART_LINE);
		return;
	}

	CHECK(!"g726_ptype is not in the channel option table");
}

/*
 * The three transfer codes. The test checks the schema class for the reason
 * that it checks fax_t38_enable and the TLS rows. Nothing in the daemon reads
 * enum cfg_support, so only the descriptor table tells an operator that the
 * daemon acts on these options.
 *
 * The test also checks the default strings. They follow the "*072" family of
 * the factory defaults, not the "*72" family. A change to "*72" would give
 * the subscriber a code that the deployed units never dialed.
 */
static void xfer_support_test(void)
{
	static const struct {
		const char *name;
		const char *def;
	} rows[] = {
		{ "blind_transfer_exten", "*072" },
		{ "attended_transfer_exten", "*073" },
		{ "consult_transfer_exten", "*074" },
	};
	const struct cfg_option_desc *d;
	unsigned int found = 0;
	unsigned int i, j;

	CASE("schema: the three transfer codes are supported, LIVE and factory seeded");
	for (i = 0; i < ARRAY_SIZE(rows); i++) {
		for (j = 0; cfg_channel_options[j].name; j++) {
			d = &cfg_channel_options[j];
			if (strcmp(d->name, rows[i].name))
				continue;

			found++;
			CHECK_UINT(d->support, SUP_SUPPORTED);
			CHECK_UINT(d->apply_class, APPLY_LIVE);
			CHECK(d->def != NULL);
			if (d->def)
				CHECK_CSTR(d->def, rows[i].def);
		}
	}
	CHECK_UINT(found, ARRAY_SIZE(rows));
}

static void xfer_tests(void)
{
	enum {
		DUMP_CHANNEL,
		DUMP_MAX,
	};
	enum {
		CHANNEL_BLIND,
		CHANNEL_ATTENDED,
		CHANNEL_CONSULT,
		CHANNEL_MAX,
	};
	static const struct blobmsg_policy dump_policy[DUMP_MAX] = {
		[DUMP_CHANNEL] = { .name = "channel", .type = BLOBMSG_TYPE_ARRAY },
	};
	static const struct blobmsg_policy channel_policy[CHANNEL_MAX] = {
		[CHANNEL_BLIND] = {
			.name = "blind_transfer_exten",
			.type = BLOBMSG_TYPE_STRING,
		},
		[CHANNEL_ATTENDED] = {
			.name = "attended_transfer_exten",
			.type = BLOBMSG_TYPE_STRING,
		},
		[CHANNEL_CONSULT] = {
			.name = "consult_transfer_exten",
			.type = BLOBMSG_TYPE_STRING,
		},
	};
	struct blob_attr *dump_fields[DUMP_MAX] = { 0 };
	struct blob_attr *channel_fields[CHANNEL_MAX] = { 0 };
	struct blob_attr *channel_entry;
	const struct cfg_channel *ch;
	struct cfg_fixture fixture;
	struct blob_buf buf = { 0 };

	CASE("explicit transfer codes load");
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(
		"\toption blind_transfer_exten '*16'\n"
		"\toption attended_transfer_exten '*17'\n"
		"\toption consult_transfer_exten '*18'\n"));
	ch = &fixture.cfg->channel[0];
	CHECK_CSTR(ch->blind_transfer_exten, "*16");
	CHECK_CSTR(ch->attended_transfer_exten, "*17");
	CHECK_CSTR(ch->consult_transfer_exten, "*18");
	fixture_free(&fixture);

	CASE("a silent config under the network default loads no transfer code");
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(""));
	ch = &fixture.cfg->channel[0];
	CHECK_CSTR(ch->blind_transfer_exten, "");
	CHECK_CSTR(ch->attended_transfer_exten, "");
	CHECK_CSTR(ch->consult_transfer_exten, "");
	fixture_free(&fixture);

	CASE("a silent config takes the three factory defaults");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n\toption star_codes_local '1'\n"
		"config channel 'channel1'\n\toption id '0'\n"));
	ch = &fixture.cfg->channel[0];
	CHECK_CSTR(ch->blind_transfer_exten, "*072");
	CHECK_CSTR(ch->attended_transfer_exten, "*073");
	CHECK_CSTR(ch->consult_transfer_exten, "*074");

	CASE("the dump carries the three transfer codes");
	blobmsg_buf_init(&buf);
	CHECK(buf.head != NULL);
	if (!buf.head) {
		blob_buf_free(&buf);
		fixture_free(&fixture);
		return;
	}

	cfg_dump(fixture.cfg, &buf);
	blobmsg_parse_attr(dump_policy, DUMP_MAX, dump_fields, buf.head);
	channel_entry = dump_fields[DUMP_CHANNEL] ?
		(struct blob_attr *)blobmsg_data(dump_fields[DUMP_CHANNEL]) : NULL;
	if (channel_entry)
		blobmsg_parse_attr(channel_policy, CHANNEL_MAX, channel_fields,
				   channel_entry);
	CHECK(channel_fields[CHANNEL_BLIND] != NULL);
	CHECK(channel_fields[CHANNEL_ATTENDED] != NULL);
	CHECK(channel_fields[CHANNEL_CONSULT] != NULL);
	if (channel_fields[CHANNEL_BLIND])
		CHECK_CSTR(blobmsg_get_string(channel_fields[CHANNEL_BLIND]), "*072");
	if (channel_fields[CHANNEL_ATTENDED])
		CHECK_CSTR(blobmsg_get_string(channel_fields[CHANNEL_ATTENDED]),
			   "*073");
	if (channel_fields[CHANNEL_CONSULT])
		CHECK_CSTR(blobmsg_get_string(channel_fields[CHANNEL_CONSULT]),
			   "*074");

	blob_buf_free(&buf);
	fixture_free(&fixture);
}

/*
 * The do not disturb pair, checked against drift as the transfer codes are.
 * *78/#78 is the own choice of uvoice (the NANP do not disturb pair). The
 * pair is not *57, because *57 is the NANP customer originated trace code.
 */
static void dnd_support_test(void)
{
	static const struct {
		const char *name;
		const char *def;
	} rows[] = {
		{ "do_not_disturb_on_exten", "*78" },
		{ "do_not_disturb_off_exten", "#78" },
	};
	const struct cfg_option_desc *d;
	unsigned int found = 0;
	unsigned int i, j;

	CASE("schema: the do not disturb pair is supported, LIVE and NANP seeded");
	for (i = 0; i < ARRAY_SIZE(rows); i++) {
		for (j = 0; cfg_channel_options[j].name; j++) {
			d = &cfg_channel_options[j];
			if (strcmp(d->name, rows[i].name))
				continue;

			found++;
			CHECK_UINT(d->support, SUP_SUPPORTED);
			CHECK_UINT(d->apply_class, APPLY_LIVE);
			CHECK(d->def != NULL);
			if (d->def)
				CHECK_CSTR(d->def, rows[i].def);
		}
	}
	CHECK_UINT(found, ARRAY_SIZE(rows));
}

/*
 * The star code loader. star_codes_local decides whether the schema defaults
 * of the 25 code rows load at all. A code that the config names loads in
 * either mode, and "none" clears one. default_tests() checks the local mode.
 */
static void star_codes_tests(void)
{
	enum {
		DUMP_STAR_CODES_LOCAL,
		DUMP_MAX,
	};
	static const struct blobmsg_policy dump_policy[DUMP_MAX] = {
		[DUMP_STAR_CODES_LOCAL] = {
			.name = "star_codes_local",
			.type = BLOBMSG_TYPE_BOOL,
		},
	};
	static const char *const modes[] = { "0", "1" };
	struct blob_attr *dump_fields[DUMP_MAX] = { 0 };
	struct cfg_fixture fixture;
	const struct cfg_channel *ch;
	struct blob_buf buf = { 0 };
	unsigned int i;

	CASE("star_codes_local defaults to the network service");
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(""));
	CHECK(!fixture.cfg->star_codes_local);

	CASE("under the default no schema code loads");
	ch = &fixture.cfg->channel[0];
	CHECK_CSTR(ch->cw_enable_code, "");
	CHECK_CSTR(ch->cw_disable_code, "");
	CHECK_CSTR(ch->cw_query_code, "");
	CHECK_CSTR(ch->cid_enable_exten, "");
	CHECK_CSTR(ch->cid_disable_exten, "");
	CHECK_CSTR(ch->cid_single_call_enable_exten, "");
	CHECK_CSTR(ch->cid_single_call_disable_exten, "");
	CHECK_CSTR(ch->do_not_disturb_on_exten, "");
	CHECK_CSTR(ch->do_not_disturb_off_exten, "");
	CHECK_CSTR(ch->call_fwd_uncond_exten, "");
	CHECK_CSTR(ch->call_fwd_uncond_cancel_exten, "");
	CHECK_CSTR(ch->call_fwd_uncond_query_exten, "");
	CHECK_CSTR(ch->call_fwd_busy_exten, "");
	CHECK_CSTR(ch->call_fwd_busy_cancel_exten, "");
	CHECK_CSTR(ch->call_fwd_busy_query_exten, "");
	CHECK_CSTR(ch->call_fwd_no_answer_exten, "");
	CHECK_CSTR(ch->call_fwd_no_answer_cancel_exten, "");
	CHECK_CSTR(ch->call_fwd_no_answer_query_exten, "");
	CHECK_CSTR(ch->call_fwd_unreachable_exten, "");
	CHECK_CSTR(ch->call_fwd_unreachable_cancel_exten, "");
	CHECK_CSTR(ch->call_fwd_all_exten, "");
	CHECK_CSTR(ch->call_fwd_all_cancel_exten, "");
	CHECK_CSTR(ch->blind_transfer_exten, "");
	CHECK_CSTR(ch->attended_transfer_exten, "");
	CHECK_CSTR(ch->consult_transfer_exten, "");

	CASE("and the services the config seeds keep their own options");
	CHECK(ch->call_fwd_local);
	CHECK_UINT(ch->call_fwd_local_mode, 0);
	CHECK(!ch->do_not_disturb_enabled);
	CHECK(!ch->cid_delivery_local_set);
	fixture_free(&fixture);

	CASE("explicit star_codes_local values load");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n\toption star_codes_local '1'\n"
		"config channel 'channel1'\n\toption id '0'\n"));
	CHECK(fixture.cfg->star_codes_local);
	CHECK_CSTR(fixture.cfg->channel[0].cw_enable_code, "*43#");
	CHECK_CSTR(fixture.cfg->channel[0].do_not_disturb_on_exten, "*78");
	fixture_free(&fixture);

	CASE("a present code loads under the network default");
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(
		"\toption do_not_disturb_on_exten '*91#'\n"
		"\toption call_waiting_enable_exten '*90#'\n"
		"\toption cancel_cw_on_exten '#90#'\n"
		"\toption blind_transfer_exten '*16'\n"));
	ch = &fixture.cfg->channel[0];
	CHECK(!fixture.cfg->star_codes_local);
	CHECK_CSTR(ch->do_not_disturb_on_exten, "*91#");
	CHECK_CSTR(ch->cw_enable_code, "*90#");
	CHECK_CSTR(ch->cw_disable_code, "#90#");
	CHECK_CSTR(ch->blind_transfer_exten, "*16");

	CASE("and its siblings stay empty");
	CHECK_CSTR(ch->do_not_disturb_off_exten, "");
	CHECK_CSTR(ch->cw_query_code, "");
	CHECK_CSTR(ch->attended_transfer_exten, "");
	fixture_free(&fixture);

	for (i = 0; i < ARRAY_SIZE(modes); i++) {
		char config[512];

		CASE("none clears a present code under each mode");
		snprintf(config, sizeof(config),
			 "config base 'base'\n\toption star_codes_local '%s'\n"
			 "config channel 'channel1'\n\toption id '0'\n"
			 "\toption call_fwd_uncond_exten 'none'\n"
			 "\toption cid_single_call_enable_exten 'none'\n"
			 "\toption blind_transfer_exten 'none'\n"
			 "\toption do_not_disturb_on_exten 'none'\n"
			 "\toption call_waiting_query_exten 'none'\n",
			 modes[i]);
		FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(config));
		ch = &fixture.cfg->channel[0];
		CHECK_CSTR(ch->call_fwd_uncond_exten, "");
		CHECK_CSTR(ch->cid_single_call_enable_exten, "");
		CHECK_CSTR(ch->blind_transfer_exten, "");
		CHECK_CSTR(ch->do_not_disturb_on_exten, "");
		CHECK_CSTR(ch->cw_query_code, "");
		fixture_free(&fixture);
	}

	CASE("none clears a call waiting code through the late alias");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n\toption star_codes_local '1'\n"
		"config channel 'channel1'\n\toption id '0'\n"
		"\toption call_waiting_enable_exten 'none'\n"));
	CHECK_CSTR(fixture.cfg->channel[0].cw_enable_code, "");
	CHECK_CSTR(fixture.cfg->channel[0].cw_disable_code, "#43#");
	fixture_free(&fixture);

	CASE("none on the cancel_cw_on_exten alias clears too");
	FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(
		"config base 'base'\n\toption star_codes_local '1'\n"
		"config channel 'channel1'\n\toption id '0'\n"
		"\toption cancel_cw_on_exten 'none'\n"
		"\toption call_waiting_disable_exten '#72#'\n"));
	CHECK_CSTR(fixture.cfg->channel[0].cw_disable_code, "");
	fixture_free(&fixture);

	CASE("NONE is not the sentinel");
	FIXTURE_LOAD_OR_RETURN(fixture, channel_option_load(
		"\toption do_not_disturb_on_exten 'NONE'\n"));
	CHECK_CSTR(fixture.cfg->channel[0].do_not_disturb_on_exten, "NONE");
	fixture_free(&fixture);

	for (i = 0; i < ARRAY_SIZE(modes); i++) {
		char config[256];

		CASE("the base dump carries star_codes_local");
		snprintf(config, sizeof(config),
			 "config base 'base'\n\toption star_codes_local '%s'\n",
			 modes[i]);
		FIXTURE_LOAD_OR_RETURN(fixture, fixture_load(config));
		blobmsg_buf_init(&buf);
		CHECK(buf.head != NULL);
		if (!buf.head) {
			blob_buf_free(&buf);
			fixture_free(&fixture);
			return;
		}
		cfg_dump(fixture.cfg, &buf);
		blobmsg_parse_attr(dump_policy, DUMP_MAX, dump_fields, buf.head);
		CHECK(dump_fields[DUMP_STAR_CODES_LOCAL] != NULL);
		if (dump_fields[DUMP_STAR_CODES_LOCAL])
			CHECK_UINT(blobmsg_get_bool(dump_fields[DUMP_STAR_CODES_LOCAL]),
				   i);
		blob_buf_free(&buf);
		fixture_free(&fixture);
	}
}

/*
 * The syslog ident carries the pid, in the shape that procd gives its stdio
 * captures. Reason: ulog opens syslog without LOG_PID, and the syslog-ng
 * template of the platform prints PROGRAM[PID]. The stdio sink prints the
 * same ident, and the capture reads it there.
 */
static void trace_ident_test(void)
{
	char want[64];

	CASE("trace_init puts the pid in the syslog ident");
	warn_capture_begin();
	trace_init("probe", false);
	ULOG_INFO("hello\n");
	snprintf(want, sizeof(want), "probe[%ld]: hello", (long)getpid());
	CHECK_UINT(warn_capture_count(want), 1);
	warn_capture_end();
}

int main(void)
{
	trace_ident_test();
	hal_schema_test();
	enum_index_schema_test();
	debug_schema_test();
	debug_tests();
	hal_default_tests();
	hal_explicit_tests();
	hal_validation_tests();
	hal_environment_tests();
	hal_dump_test();
	fax_t38_support_test();
	g726_support_test();
	xfer_support_test();
	xfer_tests();
	dnd_support_test();
	star_codes_tests();
	tls_support_test();
	transport_support_test();
	unread_support_test();
	region_schema_test();
	srtp_support_test();
	prack_support_test();
	ipv6_support_test();
	explicit_tests();
	m2_explicit_tests();
	default_tests();
	base_tests();
	control_option_tests();
	m2_invalid_tests();
	local_conf_tests();
	fwd_disc_tests();
	lec_gain_tests();
	ps_gain_tests();
	prack_tests();
	bool_malformed_tests();
	bool_spelling_tests();
	int_warn_tests();
	lec_dump_tests();
	ptime_digit_tests();
	digit_map_warn_tests();
	dtmf_tests();
	alias_tests();
	interdigit_tests();
	presence_session_tests();
	codec_tests();
	wideband_option_tests();
	port_tests();
	tls_tests();
	sip_port_presence_tests();
	field_push_tests();
	transport_enum_tests();
	protocol_tests();
	mgcp_schema_test();
	mgcp_default_tests();
	mgcp_load_tests();
	mgcp_enum_tests();
	mgcp_range_tests();
	mgcp_relation_tests();
	mgcp_pattern_tests();
	mgcp_events_tests();
	mgcp_host_tests();
	mgcp_dump_tests();
	srtp_tests();
	srtp_dump_tests();
	ipv6_tests();
	ipv6_dump_tests();
	base_dump_tests();
	channel_dump_tests();
	tls_dump_tests();
	dump_tests();
	dump_config_word_tests();
	unsetenv("UVOICE_UCI_CONFDIR");

	return tu_summary("test_cfg");
}
