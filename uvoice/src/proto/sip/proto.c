// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The SIP protocol behind struct proto_ops (proto/proto.h): the lifecycle of
 * the account layer (account.c) and the leg layer (call.c), the two account
 * verbs of the voice object, the status dumps and the wire hooks of the call
 * trace. The machines of the lines are those of proto/sip/line.c. The core
 * reaches this table through proto_ops_get() alone.
 */
#include <errno.h>
#include <string.h>

#include <libubox/blobmsg.h>
#include <libubox/utils.h>
#include <libubus.h>

#include "proto/proto.h"
#include "proto/shared/sdp.h"
#include "proto/sip/account.h"
#include "proto/sip/call.h"
#include "proto/sip/line.h"
#include "proto/sip/tap.h"
#include "proto/sip/transport.h"

/* the signaling port a channel with no config listens and sends on */
#define SIP_WIRE_PORT_DEFAULT	5060

/* The DSP readiness and the service state the leg layer admits against
 * (call.h), answered by the core. */
static bool sip_hal_ready(void)
{
	return sip_host->hal_ready(sip_host->ctx);
}

static bool sip_line_admit(unsigned int line)
{
	return sip_host->line_in_service(sip_host->ctx, line);
}

static void sip_init(const struct proto_host *host, const struct cfg *cfg)
{
	sip_lines_init(host, cfg);
	sip_wire_host = host;
	accounts_host_bind(host);

	/* calls_init() takes no config; the leg layer reads it through
	 * calls_reload(), and a call_dial before that would refuse silently */
	calls_init(&host->media, sip_line_call_cb());
	/* after calls_init(), which clears the bindings (call.h) */
	calls_host_bind(host);
	calls_reload(cfg);
	calls_hal_ready_bind(sip_hal_ready);
	calls_line_admit_bind(sip_line_admit);
}

static void sip_start(const struct cfg *cfg)
{
	accounts_init();
	accounts_start(cfg);
}

/*
 * The codec mask rides the feature push because it has the same lifetime and
 * the same probe behind it. The account layer takes the same mask: a line
 * registers over IPv6 only on a backend that carries IPv6 media.
 */
static void sip_dsp_ready(uint32_t features, uint32_t codecs)
{
	const struct hal_cap *cap = sip_host->hal_cap(sip_host->ctx);

	calls_hal_features_set(features);
	accounts_hal_features_set(features);
	calls_hal_codecs_set(codecs);
	sdp_t38_max_datagram_set(cap->t38_max_datagram);
	sip_lines_dsp_ready();
}

static void sip_reload(const struct cfg *old_cfg, const struct cfg *new_cfg)
{
	accounts_reload(old_cfg, new_cfg);
	calls_reload(new_cfg);
	sip_desk_reload(new_cfg);
}

static void sip_shutdown_begin(void)
{
	sip_lines_stop_begin();
	accounts_shutdown_begin();
}

/* The legs ended at lines_done(), so the leg layer has nothing left to end. */
static void sip_done(void)
{
	accounts_done();
	calls_done();
}

/* The SIP machine reads no late signal result: its tones and its bell do not
 * report back. */
static void sip_signal_result(unsigned int line, enum proto_signal sig,
			      bool ok)
{
}

static uint8_t sip_line_fax_kind(unsigned int line, bool *media_running)
{
	return calls_line_fax_kind(line, media_running);
}

enum {
	SIP_LINE_ARG,
	__SIP_LINE_MAX,
};

static const struct blobmsg_policy sip_line_policy[__SIP_LINE_MAX] = {
	[SIP_LINE_ARG] = { "line", BLOBMSG_TYPE_INT32 },
};

static bool sip_line_arg_get(struct blob_attr *msg, uint32_t *line)
{
	struct blob_attr *tb[__SIP_LINE_MAX] = {};

	if (msg)
		blobmsg_parse(sip_line_policy, __SIP_LINE_MAX, tb,
			      blob_data(msg), blob_len(msg));
	if (!tb[SIP_LINE_ARG])
		return false;

	*line = blobmsg_get_u32(tb[SIP_LINE_ARG]);

	return true;
}

/*
 * The two account command verbs. They answer with the line and what was asked
 * for, under "requested": the daemon accepted a command whose outcome voice
 * status reports.
 */
static int sip_ubus_register(struct ubus_context *ctx, struct ubus_object *obj,
			     struct ubus_request_data *req, const char *method,
			     struct blob_attr *msg)
{
	uint32_t line;

	if (!sip_line_arg_get(msg, &line))
		return UBUS_STATUS_INVALID_ARGUMENT;
	if (!account_register_cmd(line))
		return UBUS_STATUS_NOT_FOUND;

	sip_host->ubus_request_reply(sip_host->ctx, ctx, req, line,
				     "register");

	return 0;
}

static int sip_ubus_unregister(struct ubus_context *ctx,
			       struct ubus_object *obj,
			       struct ubus_request_data *req,
			       const char *method, struct blob_attr *msg)
{
	uint32_t line;

	if (!sip_line_arg_get(msg, &line))
		return UBUS_STATUS_INVALID_ARGUMENT;
	if (!account_unregister_cmd(line))
		return UBUS_STATUS_NOT_FOUND;

	sip_host->ubus_request_reply(sip_host->ctx, ctx, req, line,
				     "unregister");

	return 0;
}

static const struct ubus_method sip_ubus_rows[] = {
	UBUS_METHOD("register", sip_ubus_register, sip_line_policy),
	UBUS_METHOD("unregister", sip_ubus_unregister, sip_line_policy),
};

enum {
	DESK_ARG_CALL,
	DESK_ARG_CALL2,
	DESK_ARG_LINE,
	DESK_ARG_NUMBER,
	DESK_ARG_TARGET,
	DESK_ARG_PATH,
	DESK_ARG_DIGIT,
	DESK_ARG_ON,
	DESK_ARG_LEVEL,
	__DESK_ARG_MAX,
};

static const struct blobmsg_policy desk_policy[__DESK_ARG_MAX] = {
	[DESK_ARG_CALL] = { "call", BLOBMSG_TYPE_INT32 },
	[DESK_ARG_CALL2] = { "call2", BLOBMSG_TYPE_INT32 },
	[DESK_ARG_LINE] = { "line", BLOBMSG_TYPE_INT32 },
	[DESK_ARG_NUMBER] = { "number", BLOBMSG_TYPE_STRING },
	[DESK_ARG_TARGET] = { "target", BLOBMSG_TYPE_STRING },
	[DESK_ARG_PATH] = { "path", BLOBMSG_TYPE_STRING },
	[DESK_ARG_DIGIT] = { "digit", BLOBMSG_TYPE_STRING },
	[DESK_ARG_ON] = { "on", BLOBMSG_TYPE_BOOL },
	[DESK_ARG_LEVEL] = { "level", BLOBMSG_TYPE_INT32 },
};

static struct blob_buf desk_reply;

static void desk_args(struct blob_attr *msg, struct blob_attr **tb)
{
	memset(tb, 0, sizeof(*tb) * __DESK_ARG_MAX);
	if (msg)
		blobmsg_parse(desk_policy, __DESK_ARG_MAX, tb, blob_data(msg),
			      blob_len(msg));
}

static int desk_status(int ret)
{
	switch (ret) {
	case 0:
		return UBUS_STATUS_OK;
	case -ENOENT:
		return UBUS_STATUS_NOT_FOUND;
	case -EINVAL:
		return UBUS_STATUS_INVALID_ARGUMENT;
	case -ENOTSUP:
		return UBUS_STATUS_NOT_SUPPORTED;
	default:
		return UBUS_STATUS_UNKNOWN_ERROR;
	}
}

static bool desk_path_parse(struct blob_attr *a, enum hal_audio_path *path)
{
	static const char *const names[] = {
		[HAL_AUDIO_NONE] = "none",
		[HAL_AUDIO_HANDSET] = "handset",
		[HAL_AUDIO_HEADSET] = "headset",
		[HAL_AUDIO_SPEAKER] = "speaker",
		[HAL_AUDIO_HANDSET_SPEAKER] = "handset_speaker",
		[HAL_AUDIO_HEADSET_SPEAKER] = "headset_speaker",
	};
	const char *s;
	unsigned int i;

	if (!a) {
		*path = HAL_AUDIO_NONE;
		return true;
	}

	s = blobmsg_get_string(a);
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		if (!strcmp(s, names[i])) {
			*path = (enum hal_audio_path)i;
			return true;
		}
	}

	return false;
}

static int desk_ubus_dial(struct ubus_context *ctx, struct ubus_object *obj,
			  struct ubus_request_data *req, const char *method,
			  struct blob_attr *msg)
{
	struct blob_attr *tb[__DESK_ARG_MAX];
	uint32_t id = 0;
	int ret;

	desk_args(msg, tb);
	if (!tb[DESK_ARG_LINE] || !tb[DESK_ARG_NUMBER])
		return UBUS_STATUS_INVALID_ARGUMENT;

	ret = sip_desk_dial(blobmsg_get_u32(tb[DESK_ARG_LINE]),
			    blobmsg_get_string(tb[DESK_ARG_NUMBER]), &id);
	if (ret)
		return desk_status(ret);

	blob_buf_init(&desk_reply, 0);
	blobmsg_add_u32(&desk_reply, "call", id);
	ubus_send_reply(ctx, req, desk_reply.head);

	return 0;
}

static int desk_ubus_answer(struct ubus_context *ctx, struct ubus_object *obj,
			    struct ubus_request_data *req, const char *method,
			    struct blob_attr *msg)
{
	struct blob_attr *tb[__DESK_ARG_MAX];
	enum hal_audio_path path;

	desk_args(msg, tb);
	if (!tb[DESK_ARG_CALL] || !desk_path_parse(tb[DESK_ARG_PATH], &path))
		return UBUS_STATUS_INVALID_ARGUMENT;

	return desk_status(sip_desk_answer(blobmsg_get_u32(tb[DESK_ARG_CALL]),
					   path));
}

#define DESK_UBUS_CALL_VERB(name)					\
static int desk_ubus_##name(struct ubus_context *ctx,			\
			    struct ubus_object *obj,			\
			    struct ubus_request_data *req,		\
			    const char *method, struct blob_attr *msg)	\
{									\
	struct blob_attr *tb[__DESK_ARG_MAX];				\
									\
	desk_args(msg, tb);						\
	if (!tb[DESK_ARG_CALL])						\
		return UBUS_STATUS_INVALID_ARGUMENT;			\
									\
	return desk_status(sip_desk_##name(				\
		blobmsg_get_u32(tb[DESK_ARG_CALL])));			\
}

DESK_UBUS_CALL_VERB(reject)
DESK_UBUS_CALL_VERB(hangup)
DESK_UBUS_CALL_VERB(hold)
DESK_UBUS_CALL_VERB(resume)

static int desk_ubus_transfer(struct ubus_context *ctx,
			      struct ubus_object *obj,
			      struct ubus_request_data *req,
			      const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[__DESK_ARG_MAX];
	uint32_t id;

	desk_args(msg, tb);
	if (!tb[DESK_ARG_CALL])
		return UBUS_STATUS_INVALID_ARGUMENT;

	id = blobmsg_get_u32(tb[DESK_ARG_CALL]);
	if (tb[DESK_ARG_CALL2])
		return desk_status(sip_desk_transfer_attended(id,
				   blobmsg_get_u32(tb[DESK_ARG_CALL2])));
	if (tb[DESK_ARG_TARGET])
		return desk_status(sip_desk_transfer(id,
				   blobmsg_get_string(tb[DESK_ARG_TARGET])));

	return UBUS_STATUS_INVALID_ARGUMENT;
}

static int desk_ubus_conference(struct ubus_context *ctx,
				struct ubus_object *obj,
				struct ubus_request_data *req,
				const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[__DESK_ARG_MAX];

	desk_args(msg, tb);
	if (!tb[DESK_ARG_CALL] || !tb[DESK_ARG_CALL2])
		return UBUS_STATUS_INVALID_ARGUMENT;

	return desk_status(sip_desk_conference(
		blobmsg_get_u32(tb[DESK_ARG_CALL]),
		blobmsg_get_u32(tb[DESK_ARG_CALL2])));
}

static int desk_ubus_dtmf(struct ubus_context *ctx, struct ubus_object *obj,
			  struct ubus_request_data *req, const char *method,
			  struct blob_attr *msg)
{
	struct blob_attr *tb[__DESK_ARG_MAX];
	const char *digit;

	desk_args(msg, tb);
	if (!tb[DESK_ARG_DIGIT])
		return UBUS_STATUS_INVALID_ARGUMENT;

	digit = blobmsg_get_string(tb[DESK_ARG_DIGIT]);
	if (strlen(digit) != 1 || !strchr("0123456789*#ABCD", digit[0]))
		return UBUS_STATUS_INVALID_ARGUMENT;

	return desk_status(sip_desk_dtmf(digit[0]));
}

static int desk_ubus_mute(struct ubus_context *ctx, struct ubus_object *obj,
			  struct ubus_request_data *req, const char *method,
			  struct blob_attr *msg)
{
	struct blob_attr *tb[__DESK_ARG_MAX];

	desk_args(msg, tb);
	if (!tb[DESK_ARG_ON])
		return UBUS_STATUS_INVALID_ARGUMENT;

	return desk_status(sip_desk_mute(blobmsg_get_bool(tb[DESK_ARG_ON])));
}

static int desk_ubus_audio(struct ubus_context *ctx, struct ubus_object *obj,
			   struct ubus_request_data *req, const char *method,
			   struct blob_attr *msg)
{
	struct blob_attr *tb[__DESK_ARG_MAX];
	enum hal_audio_path path;

	desk_args(msg, tb);
	if (!tb[DESK_ARG_PATH] || !desk_path_parse(tb[DESK_ARG_PATH], &path))
		return UBUS_STATUS_INVALID_ARGUMENT;

	return desk_status(sip_desk_audio(path));
}

static int desk_ubus_missed(struct ubus_context *ctx, struct ubus_object *obj,
			    struct ubus_request_data *req, const char *method,
			    struct blob_attr *msg)
{
	struct blob_attr *tb[__DESK_ARG_MAX];

	desk_args(msg, tb);
	if (!tb[DESK_ARG_ON])
		return UBUS_STATUS_INVALID_ARGUMENT;

	return desk_status(sip_desk_missed(blobmsg_get_bool(tb[DESK_ARG_ON])));
}

static int desk_ubus_volume(struct ubus_context *ctx, struct ubus_object *obj,
			    struct ubus_request_data *req, const char *method,
			    struct blob_attr *msg)
{
	struct blob_attr *tb[__DESK_ARG_MAX];

	desk_args(msg, tb);
	if (!tb[DESK_ARG_LEVEL])
		return UBUS_STATUS_INVALID_ARGUMENT;

	return desk_status(sip_desk_volume(blobmsg_get_u32(tb[DESK_ARG_LEVEL])));
}

enum {
	AUDIO_ARG_HANDSET_MIC,
	AUDIO_ARG_HEADSET_MIC,
	AUDIO_ARG_SPEAKER_MIC,
	AUDIO_ARG_HANDSET_OUT,
	AUDIO_ARG_HEADSET_OUT,
	AUDIO_ARG_RING_SOUND,
	AUDIO_ARG_RING_VOLUME,
	AUDIO_ARG_DURATION,
	__AUDIO_ARG_MAX,
};

static const struct blobmsg_policy audio_policy[__AUDIO_ARG_MAX] = {
	[AUDIO_ARG_HANDSET_MIC] = { "handset_mic_gain", BLOBMSG_TYPE_INT32 },
	[AUDIO_ARG_HEADSET_MIC] = { "headset_mic_gain", BLOBMSG_TYPE_INT32 },
	[AUDIO_ARG_SPEAKER_MIC] = { "speaker_mic_gain", BLOBMSG_TYPE_INT32 },
	[AUDIO_ARG_HANDSET_OUT] = { "handset_out_gain", BLOBMSG_TYPE_INT32 },
	[AUDIO_ARG_HEADSET_OUT] = { "headset_out_gain", BLOBMSG_TYPE_INT32 },
	[AUDIO_ARG_RING_SOUND] = { "ring_sound", BLOBMSG_TYPE_STRING },
	[AUDIO_ARG_RING_VOLUME] = { "ring_volume", BLOBMSG_TYPE_INT32 },
	[AUDIO_ARG_DURATION] = { "duration_ms", BLOBMSG_TYPE_INT32 },
};

#define AUDIO_MIC_STEP_DB		2
#define AUDIO_OUT_STEP_DB		6
#define AUDIO_PREVIEW_DEFAULT_MS	3000
#define AUDIO_PREVIEW_MAX_MS		10000

static const struct {
	unsigned int arg;
	int step;
	size_t offset;
} audio_gains[] = {
	{ AUDIO_ARG_HANDSET_MIC, AUDIO_MIC_STEP_DB,
	  offsetof(struct hal_audio_tune, handset_mic_gain) },
	{ AUDIO_ARG_HEADSET_MIC, AUDIO_MIC_STEP_DB,
	  offsetof(struct hal_audio_tune, headset_mic_gain) },
	{ AUDIO_ARG_SPEAKER_MIC, AUDIO_MIC_STEP_DB,
	  offsetof(struct hal_audio_tune, speaker_mic_gain) },
	{ AUDIO_ARG_HANDSET_OUT, AUDIO_OUT_STEP_DB,
	  offsetof(struct hal_audio_tune, handset_out_gain) },
	{ AUDIO_ARG_HEADSET_OUT, AUDIO_OUT_STEP_DB,
	  offsetof(struct hal_audio_tune, headset_out_gain) },
};

static int *audio_gain_of(struct hal_audio_tune *tune, unsigned int i)
{
	return (int *)((char *)tune + audio_gains[i].offset);
}

static bool audio_gain_valid(unsigned int i, int db)
{
	const char *name = audio_policy[audio_gains[i].arg].name;
	int min, max;

	if (cfg_hal_range(name, &min, &max))
		return false;

	return db >= min && db <= max && !((db - min) % audio_gains[i].step);
}

static bool ring_sound_parse(const char *word, enum hal_ring_sound *sound)
{
	const char *const *names = cfg_ring_sound_names();
	unsigned int i;

	for (i = 0; names[i] && i < __HAL_RING_SOUND_MAX; i++) {
		if (strcmp(names[i], word))
			continue;

		*sound = (enum hal_ring_sound)i;
		return true;
	}

	return false;
}

static void audio_reply_send(struct ubus_context *ctx,
			     struct ubus_request_data *req)
{
	const char *const *names = cfg_ring_sound_names();
	struct hal_audio_tune tune;
	unsigned int i;
	void *arr;

	sip_host->audio_tune_get(sip_host->ctx, &tune);

	blob_buf_init(&desk_reply, 0);
	for (i = 0; i < ARRAY_SIZE(audio_gains); i++)
		blobmsg_add_u32(&desk_reply, audio_policy[audio_gains[i].arg].name,
				(uint32_t)*audio_gain_of(&tune, i));
	blobmsg_add_string(&desk_reply, "ring_sound", names[tune.ring_sound]);
	blobmsg_add_u32(&desk_reply, "ring_volume", sip_desk_ring_volume_get());

	arr = blobmsg_open_array(&desk_reply, "ring_sounds");
	for (i = 0; names[i] && i < __HAL_RING_SOUND_MAX; i++)
		blobmsg_add_string(&desk_reply, NULL, names[i]);
	blobmsg_close_array(&desk_reply, arr);

	ubus_send_reply(ctx, req, desk_reply.head);
}

static void audio_args(struct blob_attr *msg, struct blob_attr **tb)
{
	memset(tb, 0, sizeof(*tb) * __AUDIO_ARG_MAX);
	if (msg)
		blobmsg_parse(audio_policy, __AUDIO_ARG_MAX, tb, blob_data(msg),
			      blob_len(msg));
}

static int desk_ubus_audio_get(struct ubus_context *ctx,
			       struct ubus_object *obj,
			       struct ubus_request_data *req,
			       const char *method, struct blob_attr *msg)
{
	if (!sip_desk_active())
		return UBUS_STATUS_NOT_SUPPORTED;

	audio_reply_send(ctx, req);

	return 0;
}

static int desk_ubus_audio_set(struct ubus_context *ctx,
			       struct ubus_object *obj,
			       struct ubus_request_data *req,
			       const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[__AUDIO_ARG_MAX];
	struct hal_audio_tune tune;
	uint32_t ring_volume = 0;
	struct blob_attr *a;
	unsigned int i;
	int db;

	if (!sip_desk_active())
		return UBUS_STATUS_NOT_SUPPORTED;

	audio_args(msg, tb);
	sip_host->audio_tune_get(sip_host->ctx, &tune);

	for (i = 0; i < ARRAY_SIZE(audio_gains); i++) {
		a = tb[audio_gains[i].arg];
		if (!a)
			continue;

		db = (int32_t)blobmsg_get_u32(a);
		if (!audio_gain_valid(i, db))
			return UBUS_STATUS_INVALID_ARGUMENT;
		*audio_gain_of(&tune, i) = db;
	}

	a = tb[AUDIO_ARG_RING_SOUND];
	if (a && !ring_sound_parse(blobmsg_get_string(a), &tune.ring_sound))
		return UBUS_STATUS_INVALID_ARGUMENT;

	a = tb[AUDIO_ARG_RING_VOLUME];
	if (a) {
		ring_volume = blobmsg_get_u32(a);
		if (ring_volume > HAL_VOLUME_MAX)
			return UBUS_STATUS_INVALID_ARGUMENT;
	}

	if (sip_host->audio_tune_set(sip_host->ctx, &tune) != HAL_OK)
		return UBUS_STATUS_UNKNOWN_ERROR;
	if (a && sip_desk_ring_volume_set(ring_volume))
		return UBUS_STATUS_UNKNOWN_ERROR;

	audio_reply_send(ctx, req);

	return 0;
}

static int desk_ubus_ring_preview(struct ubus_context *ctx,
				  struct ubus_object *obj,
				  struct ubus_request_data *req,
				  const char *method, struct blob_attr *msg)
{
	uint32_t duration_ms = AUDIO_PREVIEW_DEFAULT_MS;
	struct blob_attr *tb[__AUDIO_ARG_MAX];

	audio_args(msg, tb);
	if (tb[AUDIO_ARG_DURATION])
		duration_ms = blobmsg_get_u32(tb[AUDIO_ARG_DURATION]);
	if (!duration_ms || duration_ms > AUDIO_PREVIEW_MAX_MS)
		return UBUS_STATUS_INVALID_ARGUMENT;

	return desk_status(sip_desk_ring_preview(duration_ms));
}

static int desk_ubus_ring_preview_stop(struct ubus_context *ctx,
				       struct ubus_object *obj,
				       struct ubus_request_data *req,
				       const char *method,
				       struct blob_attr *msg)
{
	return desk_status(sip_desk_ring_preview_stop());
}

static int desk_ubus_calls(struct ubus_context *ctx, struct ubus_object *obj,
			   struct ubus_request_data *req, const char *method,
			   struct blob_attr *msg)
{
	blob_buf_init(&desk_reply, 0);
	sip_desk_calls_dump(&desk_reply);
	ubus_send_reply(ctx, req, desk_reply.head);

	return 0;
}

static int desk_ubus_accounts(struct ubus_context *ctx,
			      struct ubus_object *obj,
			      struct ubus_request_data *req,
			      const char *method, struct blob_attr *msg)
{
	blob_buf_init(&desk_reply, 0);
	sip_desk_accounts_dump(&desk_reply);
	ubus_send_reply(ctx, req, desk_reply.head);

	return 0;
}

static const struct ubus_method sip_desk_rows[] = {
	UBUS_METHOD("register", sip_ubus_register, sip_line_policy),
	UBUS_METHOD("unregister", sip_ubus_unregister, sip_line_policy),
	UBUS_METHOD("dial", desk_ubus_dial, desk_policy),
	UBUS_METHOD("answer", desk_ubus_answer, desk_policy),
	UBUS_METHOD("reject", desk_ubus_reject, desk_policy),
	UBUS_METHOD("hangup", desk_ubus_hangup, desk_policy),
	UBUS_METHOD("hold", desk_ubus_hold, desk_policy),
	UBUS_METHOD("resume", desk_ubus_resume, desk_policy),
	UBUS_METHOD("transfer", desk_ubus_transfer, desk_policy),
	UBUS_METHOD("conference", desk_ubus_conference, desk_policy),
	UBUS_METHOD("dtmf", desk_ubus_dtmf, desk_policy),
	UBUS_METHOD("mute", desk_ubus_mute, desk_policy),
	UBUS_METHOD("audio", desk_ubus_audio, desk_policy),
	UBUS_METHOD("volume", desk_ubus_volume, desk_policy),
	UBUS_METHOD("missed", desk_ubus_missed, desk_policy),
	UBUS_METHOD_NOARG("calls", desk_ubus_calls),
	UBUS_METHOD_NOARG("accounts", desk_ubus_accounts),
	UBUS_METHOD_NOARG("audio_get", desk_ubus_audio_get),
	UBUS_METHOD("audio_set", desk_ubus_audio_set, audio_policy),
	UBUS_METHOD("ring_preview", desk_ubus_ring_preview, audio_policy),
	UBUS_METHOD_NOARG("ring_preview_stop", desk_ubus_ring_preview_stop),
};

static const struct ubus_method *sip_ubus_methods(unsigned int *n)
{
	if (sip_desk_active()) {
		*n = ARRAY_SIZE(sip_desk_rows);
		return sip_desk_rows;
	}

	*n = ARRAY_SIZE(sip_ubus_rows);

	return sip_ubus_rows;
}

static void sip_status_dump_call(struct blob_buf *b, unsigned int line)
{
	sip_line_status_dump(b, line);
	calls_status_dump(b, line);
}

static void sip_status_dump_media(struct blob_buf *b)
{
	blobmsg_add_u32(b, "opus_dtmf_fallbacks", calls_opus_dtmf_fallbacks());
}

/*
 * The capture filter ports of one line: the local SIP port catches every
 * exchange whatever port the far end sends from, and the configured remote
 * port catches an outbound stream connection from an ephemeral local port.
 */
static unsigned int sip_wire_ports(unsigned int line, uint16_t *ports,
				   unsigned int max)
{
	const struct cfg *cfg = sip_host->cfg(sip_host->ctx);
	uint16_t local = SIP_WIRE_PORT_DEFAULT;
	uint16_t remote = SIP_WIRE_PORT_DEFAULT;
	unsigned int n = 0;

	if (cfg && line < CFG_CHANNEL_MAX) {
		local = cfg->channel[line].sip_local_port;
		remote = cfg->channel[line].sip_port;
	}

	if (n < max)
		ports[n++] = local;
	if (remote != local && n < max)
		ports[n++] = remote;

	return n;
}

/* The datagram pool's own counters. The pool serves every line, so these are
 * the daemon's totals and not the traced line's. */
static void sip_wire_stats(struct blob_buf *b)
{
	const struct sip_sock_stat *st = sip_transport_stat(acct_transport());
	void *tbl;

	if (!st)
		return;

	tbl = blobmsg_open_table(b, "sip");
	blobmsg_add_u64(b, "rx", st->rx);
	blobmsg_add_u64(b, "rx_bytes", st->rx_bytes);
	blobmsg_add_u64(b, "tx", st->tx);
	blobmsg_add_u64(b, "tx_bytes", st->tx_bytes);
	blobmsg_add_u64(b, "tx_err", st->tx_err);
	blobmsg_add_u64(b, "rx_err", st->rx_err);
	blobmsg_add_u64(b, "parse_err", st->parse_err);
	blobmsg_add_u64(b, "truncated", st->truncated);
	blobmsg_add_u64(b, "ping", st->ping);
	blobmsg_close_table(b, tbl);
}

const struct proto_ops sip_proto_ops = {
	.name = "sip",
	.init = sip_init,
	.start = sip_start,
	.dsp_ready = sip_dsp_ready,
	.reload = sip_reload,
	.shutdown_begin = sip_shutdown_begin,
	.shutdown_pending = accounts_shutdown_pending,
	.done = sip_done,
	.line_event = sip_line_event,
	.line_service = sip_line_service,
	.line_reset = sip_line_reset,
	.line_cfg_changed = sip_line_cfg_changed,
	.region_changed = sip_line_region_changed,
	.ring_held = sip_line_ring_held,
	.ring_started = sip_line_ring_started,
	.ring_failed = sip_line_ring_failed,
	.signal_result = sip_signal_result,
	.line_flags = sip_line_flags,
	.state_name = sip_line_state_name,
	.digit_play_allowed = sip_line_digit_play_allowed,
	.stream_of_slot = sip_line_stream_of_slot,
	.line_fax_kind = sip_line_fax_kind,
	.ubus_methods = sip_ubus_methods,
	.status_dump_call = sip_status_dump_call,
	.status_dump_lines = accounts_status_dump,
	.status_dump_media = sip_status_dump_media,
	.wire_ports = sip_wire_ports,
	.tap_set = sip_tap_set,
	.wire_stats = sip_wire_stats,
};
