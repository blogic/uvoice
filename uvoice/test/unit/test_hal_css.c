// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Unit tests for the css HAL backend (src/hal/css/css.c). The real backend is
 * compiled into this binary and a case drives hal_backend_ops end to end. The
 * lifecycle cases point the backend's sysfs and device paths at a temporary
 * directory (css_rproc_state_path and friends), so dsp_start runs without the
 * real remoteproc. The media, tone, input and audio cases are added with those
 * paths.
 */
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libubox/uloop.h>

#include "hal/hal.h"
#include "hal/css/css_int.h"
#include "trace.h"

#include "test_util.h"
#include "test_uloop.h"

static char g_dir[64];
static char g_state[128];
static char g_fw[128];
static char g_dev[128];

static void file_write(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

	if (fd < 0)
		return;
	if (write(fd, s, strlen(s)) < 0)
		(void)0;
	close(fd);
}

static void paths_init(void)
{
	snprintf(g_dir, sizeof(g_dir), "/tmp/uvoice-css-XXXXXX");
	if (!mkdtemp(g_dir)) {
		perror("mkdtemp");
		exit(1);
	}
	snprintf(g_state, sizeof(g_state), "%s/state", g_dir);
	snprintf(g_fw, sizeof(g_fw), "%s/css-loader", g_dir);
	snprintf(g_dev, sizeof(g_dev), "%s/css-voice", g_dir);
	css_rproc_state_path = g_state;
	css_firmware_path = g_fw;
	css_voice_dev_path = g_dev;
	/* Keep the input, audio and tone planes off the host hardware: an empty
	 * input directory enumerates no evdev device, and a card and PCM that do
	 * not exist make the audio and tone opens log and carry on. */
	css_input_dir = g_dir;
	css_led_dir = g_dir;
	css_card_name = "hw:CssTestNoCard";
	css_pcm_name = "hw:CssTestNoCard";
}

static void paths_cleanup(void)
{
	unlink(g_state);
	unlink(g_fw);
	unlink(g_dev);
	rmdir(g_dir);
}

struct probe {
	struct hal *h;
	int rx_fd;
	int tx_fd;
};

static bool probe_open(struct probe *p)
{
	int fd[2];

	memset(p, 0, sizeof(*p));
	p->rx_fd = p->tx_fd = -1;

	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, fd))
		return false;

	p->rx_fd = fd[0];
	p->tx_fd = fd[1];
	p->h = hal_backend_ops.open(p->tx_fd);
	if (p->h)
		return true;

	close(p->rx_fd);
	close(p->tx_fd);
	p->rx_fd = p->tx_fd = -1;

	return false;
}

static void probe_close(struct probe *p)
{
	if (p->h)
		hal_backend_ops.close(p->h);
	p->h = NULL;

	if (p->rx_fd >= 0)
		close(p->rx_fd);
	if (p->tx_fd >= 0)
		close(p->tx_fd);
	p->rx_fd = p->tx_fd = -1;
}

/* One record off the core end of the event socket, false when none waits. */
static bool ev_read(struct probe *p, struct hal_event *out)
{
	return recv(p->rx_fd, out, sizeof(*out), MSG_DONTWAIT) ==
	       (ssize_t)sizeof(*out);
}

static void test_caps(void)
{
	const uint32_t want = HAL_FEAT_DESK | HAL_FEAT_CONF | HAL_FEAT_SRTP |
			      HAL_FEAT_WIDEBAND | HAL_FEAT_G729 | HAL_FEAT_G726 |
			      HAL_FEAT_IPV6_MEDIA | HAL_FEAT_RTCP_ADDR;
	const uint32_t codecs = (1u << HAL_CODEC_PCMU) | (1u << HAL_CODEC_PCMA) |
				(1u << HAL_CODEC_G722) | (1u << HAL_CODEC_G729) |
				(1u << HAL_CODEC_G726_32);
	const struct hal_cap *cap;
	struct probe p;

	CASE("open attaches and publishes the desk capability");
	CHECK(probe_open(&p));
	if (!p.h)
		return;
	cap = hal_backend_ops.cap_get(p.h);
	CHECK(!strcmp(cap->backend, "css"));
	CHECK_UINT(cap->lines, 2);
	CHECK_UINT(cap->streams_per_chan, 2);
	CHECK_UINT(cap->features, want);
	CHECK(!(cap->features & HAL_FEAT_DTMF_RELAY));
	CHECK(!(cap->features & HAL_FEAT_DTMF_PLAYOUT));
	CHECK_UINT(cap->codecs, codecs);
	probe_close(&p);
}

static bool job_done_read(struct probe *p, uint32_t token, int32_t *status)
{
	struct hal_event ev;

	if (!ev_read(p, &ev))
		return false;
	if (ev.type != HAL_EVENT_JOB_DONE || ev.u.job.token != token)
		return false;
	*status = ev.u.job.status;

	return true;
}

static void test_dsp_start_running(void)
{
	struct hal_job job = { 0 };
	int32_t status = -1;
	struct probe p;

	CASE("dsp_start completes at once when the CSS already runs");
	file_write(g_state, "running\n");
	file_write(g_dev, "");
	CHECK(probe_open(&p));
	if (!p.h)
		return;
	CHECK(hal_backend_ops.dsp_start(p.h, &job) == HAL_OK);
	CHECK(job_done_read(&p, job.token, &status));
	CHECK_UINT(status, HAL_OK);
	probe_close(&p);
}

static void test_dsp_start_no_firmware(void)
{
	struct hal_job job = { 0 };
	struct probe p;

	CASE("dsp_start fails when the CSS is offline and no firmware is present");
	file_write(g_state, "offline\n");
	unlink(g_fw);
	CHECK(probe_open(&p));
	if (!p.h)
		return;
	CHECK(hal_backend_ops.dsp_start(p.h, &job) == HAL_FATAL);
	probe_close(&p);
}

static void test_dsp_start_poll(void)
{
	struct hal_job job = { 0 };
	int32_t status = -1;
	struct probe p;

	CASE("dsp_start asks for a load and completes when the CSS comes up");
	file_write(g_state, "offline\n");
	file_write(g_fw, "x");
	unlink(g_dev);
	CHECK(probe_open(&p));
	if (!p.h)
		return;
	CHECK(hal_backend_ops.dsp_start(p.h, &job) == HAL_OK);
	/* The backend asked remoteproc to load; model the CSS coming up. */
	file_write(g_state, "running\n");
	file_write(g_dev, "");
	uloop_pump(CSS_START_POLL_MS * 3);
	CHECK(job_done_read(&p, job.token, &status));
	CHECK_UINT(status, HAL_OK);
	probe_close(&p);
}

static void test_dsp_stop(void)
{
	struct hal_job job = { 0 };
	int32_t status = -1;
	struct probe p;

	CASE("dsp_stop completes and leaves the CSS running");
	file_write(g_state, "running\n");
	file_write(g_dev, "");
	CHECK(probe_open(&p));
	if (!p.h)
		return;
	CHECK(hal_backend_ops.dsp_stop(p.h, &job) == HAL_OK);
	CHECK(job_done_read(&p, job.token, &status));
	CHECK_UINT(status, HAL_OK);
	probe_close(&p);
}

/* --- the media path, against a fake /dev/css-voice ------------------------ */

struct fake_dev {
	struct dvf99_voice_config start;
	struct dvf99_voice_config update;
	struct dvf99_voice_dtmf dtmf;
	struct dvf99_voice_stats stats;
	int started;
	int updated;
	int stopped;
	int dtmf_n;
	int open_line;
};

static struct fake_dev g_fake;

static int fk_open(void *ctx, unsigned int line)
{
	struct fake_dev *f = ctx;

	f->open_line = (int)line;
	return 100;
}

static int fk_ioctl(void *ctx, int fd, unsigned long req, void *arg)
{
	struct fake_dev *f = ctx;

	if (req == DVF99_VOICE_START) {
		f->start = *(const struct dvf99_voice_config *)arg;
		f->started++;
	} else if (req == DVF99_VOICE_UPDATE) {
		f->update = *(const struct dvf99_voice_config *)arg;
		f->updated++;
	} else if (req == DVF99_VOICE_STOP) {
		f->stopped++;
	} else if (req == DVF99_VOICE_DTMF) {
		f->dtmf = *(const struct dvf99_voice_dtmf *)arg;
		f->dtmf_n++;
	} else if (req == DVF99_VOICE_STATS) {
		*(struct dvf99_voice_stats *)arg = f->stats;
	} else {
		return -1;
	}

	return 0;
}

static long fk_read(void *ctx, int fd, void *buf, unsigned long len)
{
	return -1;
}

static long fk_write(void *ctx, int fd, const void *buf, unsigned long len)
{
	return (long)len;
}

static void fk_close(void *ctx, int fd)
{
}

static const struct css_devops fake_ops = {
	.open = fk_open,
	.xioctl = fk_ioctl,
	.xread = fk_read,
	.xwrite = fk_write,
	.xclose = fk_close,
	.ctx = &g_fake,
};

static bool media_probe(struct probe *p)
{
	if (!probe_open(p))
		return false;
	memset(&g_fake, 0, sizeof(g_fake));
	p->h->dev = &fake_ops;

	return true;
}

static void tm_media(struct hal_media *m, uint8_t codec, uint8_t dir)
{
	memset(m, 0, sizeof(*m));
	m->local.ipver = 4;
	m->local.addr[0] = 127;
	m->local.addr[3] = 1;
	m->local.port = 51000;
	m->remote = m->local;
	m->remote.port = 52000;
	m->codec = codec;
	m->codec_pt = 0;
	m->dtmf_pt = 101;
	m->ptime_ms = 20;
	m->direction = dir;
	m->jb_mode = HAL_JB_ADAPTIVE;
	m->rtcp_enable = true;
	m->rtcp_interval_ms = 5000;
}

static void test_media_map(void)
{
	struct hal_media m;
	struct probe p;

	CASE("stream_config_set maps the description and start pushes it");
	if (!media_probe(&p))
		return;
	tm_media(&m, HAL_CODEC_PCMU, HAL_MEDIA_SENDRECV);
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_OK);
	CHECK_UINT(g_fake.started, 0);
	CHECK(hal_backend_ops.stream_start(p.h, 0, 0) == HAL_OK);
	CHECK_UINT(g_fake.started, 1);
	CHECK_UINT(g_fake.start.line, 0);
	CHECK_UINT(g_fake.start.codec, DVF99_VOICE_CODEC_PCMU);
	CHECK_UINT(g_fake.start.tx_pt, 0);
	CHECK_UINT(g_fake.start.rx_pt, 0);
	CHECK_UINT(g_fake.start.tx_pt_event, 101);
	CHECK_UINT(g_fake.start.direction, DVF99_VOICE_DIR_SENDRECV);
	CHECK(g_fake.start.flags & DVF99_VOICE_F_RTCP);
	CHECK(!(g_fake.start.flags & DVF99_VOICE_F_DETACHED));
	CHECK(g_fake.start.ssrc != 0);
	CHECK(g_fake.start.cname[0] != 0);
	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);
	CHECK_UINT(g_fake.stopped, 1);
	probe_close(&p);
}

static void detach_check(const char *name, uint8_t dir, bool tx_silence)
{
	struct hal_media m;
	struct probe p;

	CASE(name);
	if (!media_probe(&p))
		return;
	tm_media(&m, HAL_CODEC_PCMU, dir);
	m.tx_silence = tx_silence;
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_OK);
	CHECK(hal_backend_ops.stream_start(p.h, 0, 0) == HAL_OK);
	CHECK(g_fake.start.flags & DVF99_VOICE_F_DETACHED);
	probe_close(&p);
}

static void test_media_detach(void)
{
	detach_check("a send-only stream is detached from the audio path",
		     HAL_MEDIA_SENDONLY, false);
	detach_check("a held stream (tx_silence) is detached",
		     HAL_MEDIA_SENDRECV, true);
}

static void test_media_reconfig(void)
{
	struct hal_media m;
	struct probe p;
	uint32_t ssrc;

	CASE("a re-INVITE updates the running stream and keeps its identity");
	if (!media_probe(&p))
		return;
	tm_media(&m, HAL_CODEC_PCMU, HAL_MEDIA_SENDRECV);
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_OK);
	CHECK(hal_backend_ops.stream_start(p.h, 0, 0) == HAL_OK);
	ssrc = g_fake.start.ssrc;
	/* The far end moved; the local tuple is unchanged, so the pair is kept
	 * and the CSS gets an UPDATE, not a fresh START. */
	m.remote.port = 53000;
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_OK);
	CHECK_UINT(g_fake.updated, 1);
	CHECK_UINT(g_fake.started, 1);
	CHECK_UINT(g_fake.update.ssrc, ssrc);
	probe_close(&p);
}

static void test_media_dtmf(void)
{
	struct hal_media m;
	struct probe p;

	CASE("stream_dtmf_send issues the RFC 4733 event to the CSS");
	if (!media_probe(&p))
		return;
	tm_media(&m, HAL_CODEC_PCMU, HAL_MEDIA_SENDRECV);
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_OK);
	CHECK(hal_backend_ops.stream_start(p.h, 0, 0) == HAL_OK);
	CHECK(hal_backend_ops.stream_dtmf_send(p.h, 0, 0, '5', 100) == HAL_OK);
	CHECK_UINT(g_fake.dtmf_n, 1);
	CHECK_UINT(g_fake.dtmf.event, 5);
	CHECK_UINT(g_fake.dtmf.duration_ms, 100);
	CHECK(hal_backend_ops.stream_dtmf_send(p.h, 0, 0, '#', 80) == HAL_OK);
	CHECK_UINT(g_fake.dtmf.event, 11);
	probe_close(&p);
}

static void test_media_stats(void)
{
	struct hal_media_stats out;
	struct hal_media m;
	struct probe p;

	CASE("stream_stats_get maps the device counters");
	if (!media_probe(&p))
		return;
	tm_media(&m, HAL_CODEC_PCMU, HAL_MEDIA_SENDRECV);
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_OK);
	CHECK(hal_backend_ops.stream_start(p.h, 0, 0) == HAL_OK);
	g_fake.stats.pkts_sent = 42;
	g_fake.stats.pkts_recvd = 40;
	g_fake.stats.pkts_lost = 5;
	g_fake.stats.pkts_dropped = 0xffffffffu;	/* not counted */
	g_fake.stats.jitter = 800;
	g_fake.stats.rtt = 0xffffffffu;			/* not measured */
	CHECK(hal_backend_ops.stream_stats_get(p.h, 0, 0, &out) == HAL_OK);
	CHECK_UINT(out.packets_sent, 42);
	CHECK_UINT(out.packets_recv, 40);
	CHECK_UINT(out.packets_lost, 5);
	CHECK_UINT(out.packets_discarded, 0);	/* 0xffffffff maps to absent */
	CHECK_UINT(out.jitter_avg, 800);	/* the CSS jitter, mapped through */
	CHECK_UINT(out.rtt_avg, 0);
	probe_close(&p);
}

static void test_media_refusals(void)
{
	struct hal_media m;
	struct probe p;

	CASE("an unknown codec and an unknown srtp suite are refused");
	if (!media_probe(&p))
		return;
	tm_media(&m, HAL_CODEC_T38, HAL_MEDIA_SENDRECV);
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_UNSUPPORTED);
	tm_media(&m, HAL_CODEC_PCMU, HAL_MEDIA_SENDRECV);
	m.srtp.suite = 3;
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_UNSUPPORTED);
	probe_close(&p);
}

static void test_media_srtp(void)
{
	struct hal_media m;
	struct probe p;
	unsigned int i;

	CASE("an SRTP stream configures and starts");
	if (!media_probe(&p))
		return;
	tm_media(&m, HAL_CODEC_PCMU, HAL_MEDIA_SENDRECV);
	m.srtp.suite = HAL_SRTP_AES_CM_128_SHA1_80;
	for (i = 0; i < 30; i++) {
		m.srtp.key_tx[i] = (uint8_t)(0x10 + i);
		m.srtp.key_rx[i] = (uint8_t)(0x40 + i);
	}
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_OK);
	CHECK(hal_backend_ops.stream_start(p.h, 0, 0) == HAL_OK);
	CHECK_UINT(g_fake.started, 1);
	probe_close(&p);
}

/* --- the packet thread, against a socketpair device and a loopback peer ---- */

struct thread_fake {
	int dev_backend;	/* the fd the backend polls and reads/writes */
	int dev_test;		/* the CSS side, held by the test */
};

static struct thread_fake g_tf;

static int tf_open(void *ctx, unsigned int line)
{
	struct thread_fake *f = ctx;
	int fd[2];

	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, fd))
		return -1;
	f->dev_backend = fd[0];
	f->dev_test = fd[1];

	return fd[0];
}

static int tf_ioctl(void *ctx, int fd, unsigned long req, void *arg)
{
	return 0;
}

static long tf_read(void *ctx, int fd, void *buf, unsigned long len)
{
	return read(fd, buf, len);
}

static long tf_write(void *ctx, int fd, const void *buf, unsigned long len)
{
	return write(fd, buf, len);
}

static void tf_close(void *ctx, int fd)
{
	struct thread_fake *f = ctx;

	if (fd >= 0)
		close(fd);
	if (f->dev_test >= 0)
		close(f->dev_test);
	f->dev_backend = f->dev_test = -1;
}

static const struct css_devops thread_ops = {
	.open = tf_open,
	.xioctl = tf_ioctl,
	.xread = tf_read,
	.xwrite = tf_write,
	.xclose = tf_close,
	.ctx = &g_tf,
};

static int loopback_bind(uint16_t port)
{
	struct sockaddr_in sa;
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);

	if (fd < 0)
		return -1;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(fd, (void *)&sa, sizeof(sa))) {
		close(fd);
		return -1;
	}

	return fd;
}

static bool fd_ready(int fd, int ms)
{
	struct pollfd pf = { .fd = fd, .events = POLLIN };

	return poll(&pf, 1, ms) == 1 && (pf.revents & POLLIN);
}

static void test_pkt_forward(void)
{
	static const uint8_t rtp[20] = {
		0x80, 0x00, 0x12, 0x34, 0x00, 0x00, 0x03, 0xe8,
		0xca, 0xfe, 0xba, 0xbe, 0x11, 0x22, 0x33, 0x44,
		0x55, 0x66, 0x77, 0x88,
	};
	uint8_t pktbuf[sizeof(struct dvf99_voice_pkt) + sizeof(rtp)];
	uint8_t rec[2000];
	struct dvf99_voice_pkt *ip = (void *)pktbuf;
	struct dvf99_voice_pkt *dp = (void *)rec;
	struct sockaddr_in to;
	struct hal_media m;
	struct probe p;
	int far;

	CASE("the packet thread forwards RTP between the device and the socket");
	if (!probe_open(&p))
		return;
	g_tf.dev_backend = g_tf.dev_test = -1;
	p.h->dev = &thread_ops;

	far = loopback_bind(52000);
	CHECK(far >= 0);
	tm_media(&m, HAL_CODEC_PCMU, HAL_MEDIA_SENDRECV);
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_OK);
	CHECK(hal_backend_ops.stream_start(p.h, 0, 0) == HAL_OK);

	/* device -> socket: a record the CSS "produced" reaches the far end. */
	ip->type = DVF99_VOICE_PKT_RTP;
	ip->ttl = 64;
	ip->time_ms = 0;
	memcpy(ip->data, rtp, sizeof(rtp));
	CHECK(write(g_tf.dev_test, pktbuf, sizeof(pktbuf)) == (ssize_t)sizeof(pktbuf));
	CHECK(fd_ready(far, 1000));
	CHECK_UINT(recv(far, rec, sizeof(rec), 0), sizeof(rtp));
	CHECK(memcmp(rec, rtp, sizeof(rtp)) == 0);

	/* socket -> device: a datagram from the far end reaches the CSS, framed
	 * as a dvf99_voice_pkt. */
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_port = htons(51000);
	to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	CHECK(sendto(far, rtp, sizeof(rtp), 0, (void *)&to, sizeof(to)) ==
	      (ssize_t)sizeof(rtp));
	CHECK(fd_ready(g_tf.dev_test, 1000));
	{
		ssize_t n = recv(g_tf.dev_test, rec, sizeof(rec), 0);

		CHECK(n >= (ssize_t)sizeof(*dp));
		CHECK_UINT(dp->type, DVF99_VOICE_PKT_RTP);
		CHECK_UINT((size_t)n - sizeof(*dp), sizeof(rtp));
		CHECK(memcmp(dp->data, rtp, sizeof(rtp)) == 0);
	}

	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);
	close(far);
	probe_close(&p);
}

static void test_jitter(void)
{
	struct css_stream s;
	unsigned int i;

	CASE("rfc 3550 jitter: constant pacing is zero, an offset accumulates");
	memset(&s, 0, sizeof(s));
	for (i = 0; i < 20; i++)
		css_rtp_jitter_update(&s, i * 160u, 1000u + i * 160u);
	CHECK_UINT(s.jit, 0);

	memset(&s, 0, sizeof(s));
	css_rtp_jitter_update(&s, 0, 0);
	css_rtp_jitter_update(&s, 160, 160 + 80);
	CHECK_UINT(s.jit, 80);
	css_rtp_jitter_update(&s, 320, 320);
	CHECK_UINT(s.jit, 155);
}

int main(void)
{
	uloop_init();
	paths_init();

	test_caps();
	test_dsp_start_running();
	test_dsp_start_no_firmware();
	test_dsp_start_poll();
	test_dsp_stop();
	test_media_map();
	test_media_detach();
	test_media_reconfig();
	test_media_dtmf();
	test_media_stats();
	test_media_refusals();
	test_media_srtp();
	test_pkt_forward();
	test_jitter();

	paths_cleanup();
	uloop_done();

	return tu_summary("test_hal_css");
}
