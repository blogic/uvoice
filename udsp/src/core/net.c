// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * The net thread. See core/net.h for the contract.
 */
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "net.h"
#include "udsp_priv.h"
#include "stream.h"


static pthread_once_t net_ack_once = PTHREAD_ONCE_INIT;
static unsigned net_ack_ms = UDSP_NET_ACK_MS;

/* A zero or malformed value keeps the production deadline, so the override
 * never disables the ack wait. */
static void net_ack_ms_init(void)
{
	const char *v = getenv("UDSP_NET_ACK_MS");
	char *end = NULL;
	unsigned long n;

	if (!v || *v < '0' || *v > '9')
		return;
	errno = 0;
	n = strtoul(v, &end, 10);
	if (errno || end == v || (end && *end) || n == 0 || n > UINT_MAX)
		return;

	net_ack_ms = (unsigned)n;
}

unsigned net_ack_ms_get(void)
{
	pthread_once(&net_ack_once, net_ack_ms_init);

	return net_ack_ms;
}

static void net_count(_Atomic uint_least64_t *counter, uint64_t n)
{
	atomic_fetch_add_explicit(counter, n, memory_order_relaxed);
}

/* --- the receive path ---------------------------------------------------- */

/* A datagram above the slot, or one the receive session refuses, is counted
 * and dropped with an empty slot: the engine must never parse a fragment. */
static void net_slot_fill(struct udsp_stream *st, struct net_slot *slot,
			  unsigned len, unsigned kind)
{
	size_t out = 0;
	int rc;

	slot->kind = (uint8_t)kind;
	slot->arrival_ns = udsp_mono_ns();
	if (len > UDSP_NET_DGRAM_MAX) {
		net_count(&st->net_trunc, 1u);
		slot->len = 0;
		return;
	}
	if (!atomic_load_explicit(&st->srtp_armed, memory_order_acquire)) {
		slot->len = len;
		return;
	}

	if (kind == NET_KIND_RTCP)
		rc = uvsrtp_unprotect_rtcp(st->srtp, slot->data, len,
					   sizeof(slot->data), &out);
	else
		rc = uvsrtp_unprotect(st->srtp, slot->data, len,
				      sizeof(slot->data), &out);
	if (rc) {
		net_count(&st->net_trunc, 1u);
		slot->len = 0;
		return;
	}
	slot->len = (uint32_t)out;
}

/*
 * The kind of one received datagram (udsp.h enum udsp_pkt_kind). The
 * registration tells the report socket from the media socket. On the media
 * socket a T.38 session carries UDPTL where an audio stream carries RTP, and
 * the published mode is the one field of the engine's chain this thread may
 * read (core/stream.h net_mode).
 */
static unsigned net_slot_kind(const struct udsp_stream *st, unsigned kind)
{
	if (kind == NET_KIND_RTCP)
		return UDSP_PKT_RTCP;
	if (atomic_load_explicit(&st->net_mode, memory_order_relaxed) ==
	    STREAM_MODE_T38)
		return UDSP_PKT_UDPTL;

	return UDSP_PKT_RTP;
}

/*
 * One receive record per slot the batch filled, taken before the head that
 * publishes the batch, so the record carries the plaintext net_slot_fill left:
 * the unprotect of an SRTP session runs on this thread and in place.
 *
 * The gate is read once per batch, the ring pointer first, which is NULL for
 * every run that never armed the trace.
 */
static void net_tap_batch(struct udsp *s, const struct udsp_stream *st,
			  unsigned head, unsigned count)
{
	struct pkt_trace_ring *r = atomic_load_explicit(&s->pkt_rx,
							memory_order_acquire);
	struct pkt_trace_meta m;

	if (!r || !(atomic_load_explicit(&s->debug, memory_order_relaxed) &
		    UDSP_DEBUG_PKT_TRACE))
		return;

	m.gen = (uint32_t)atomic_load_explicit(&st->gen, memory_order_relaxed);
	m.line = st->line;
	m.stream = st->index;
	m.dir = UDSP_PKT_RX;
	for (unsigned i = 0; i < count; i++) {
		const struct net_slot *slot =
			&st->ring.slot[(head + i) % UDSP_NET_SLOTS];

		m.stamp_ns = slot->arrival_ns;
		m.kind = net_slot_kind(st, slot->kind);
		pkt_trace_push(r, &m, slot->data, slot->len);
	}
}

/* The socket must be drained even with nowhere to put the payload, or a
 * level-triggered epoll set would wake on the same backlog for ever. */
static void net_discard(int fd, struct udsp_stream *st)
{
	uint8_t sink[UDSP_NET_DGRAM_MAX];

	for (unsigned i = 0; i < UDSP_NET_BATCH; i++) {
		ssize_t n = recv(fd, sink, sizeof(sink), MSG_DONTWAIT | MSG_TRUNC);

		if (n < 0)
			return;
		net_count(&st->net_drops, 1u);
	}
}

static void net_stream_recv(struct udsp *s, struct udsp_stream *st, int fd,
			    unsigned kind)
{
	for (;;) {
		struct mmsghdr msgs[UDSP_NET_BATCH];
		struct iovec iov[UDSP_NET_BATCH];
		unsigned head, tail, room, want;
		int n;

		head = atomic_load_explicit(&st->ring.head, memory_order_relaxed);
		tail = atomic_load_explicit(&st->ring.tail, memory_order_acquire);
		room = UDSP_NET_SLOTS - (head - tail);
		if (!room) {
			net_discard(fd, st);
			return;
		}
		want = room < UDSP_NET_BATCH ? room : UDSP_NET_BATCH;

		memset(msgs, 0, sizeof(msgs));
		for (unsigned i = 0; i < want; i++) {
			struct net_slot *slot =
				&st->ring.slot[(head + i) % UDSP_NET_SLOTS];

			iov[i].iov_base = slot->data;
			iov[i].iov_len = sizeof(slot->data);
			msgs[i].msg_hdr.msg_iov = &iov[i];
			msgs[i].msg_hdr.msg_iovlen = 1;
		}
		n = recvmmsg(fd, msgs, want, MSG_DONTWAIT | MSG_TRUNC, NULL);
		if (n <= 0)
			return;

		for (unsigned i = 0; i < (unsigned)n; i++)
			net_slot_fill(st,
				      &st->ring.slot[(head + i) % UDSP_NET_SLOTS],
				      msgs[i].msg_len, kind);
		net_tap_batch(s, st, head, (unsigned)n);
		atomic_store_explicit(&st->ring.head, head + (unsigned)n,
				      memory_order_release);
		if ((unsigned)n < want)
			return;
	}
}

/* --- the descriptor set -------------------------------------------------- */

static void net_epoll_drop(struct udsp *s, struct udsp_stream *st)
{
	if (!st->attached)
		return;

	if (st->net_rtp_fd >= 0)
		epoll_ctl(s->net.epoll_fd, EPOLL_CTL_DEL, st->net_rtp_fd, NULL);
	if (st->net_rtcp_fd >= 0)
		epoll_ctl(s->net.epoll_fd, EPOLL_CTL_DEL, st->net_rtcp_fd, NULL);
	st->net_rtp_fd = -1;
	st->net_rtcp_fd = -1;
	st->attached = false;
}

static int net_epoll_add(struct udsp *s, struct udsp_stream *st, int fd,
			 unsigned kind)
{
	struct epoll_event ev;

	if (fd < 0)
		return 0;

	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = &st->reg[kind];

	return epoll_ctl(s->net.epoll_fd, EPOLL_CTL_ADD, fd, &ev) ? -errno : 0;
}

static int net_cmd_attach(struct udsp *s, struct udsp_stream *st,
			  const struct net_cmd *cmd)
{
	int rc;

	net_epoll_drop(s, st);
	st->reg[NET_KIND_RTP].stream = st;
	st->reg[NET_KIND_RTP].kind = NET_KIND_RTP;
	st->reg[NET_KIND_RTCP].stream = st;
	st->reg[NET_KIND_RTCP].kind = NET_KIND_RTCP;

	rc = net_epoll_add(s, st, cmd->rtp_fd, NET_KIND_RTP);
	if (rc)
		return rc;
	rc = net_epoll_add(s, st, cmd->rtcp_fd, NET_KIND_RTCP);
	if (rc) {
		if (cmd->rtp_fd >= 0)
			epoll_ctl(s->net.epoll_fd, EPOLL_CTL_DEL, cmd->rtp_fd,
				  NULL);
		return rc;
	}
	st->net_rtp_fd = cmd->rtp_fd;
	st->net_rtcp_fd = cmd->rtcp_fd;
	st->attached = true;

	return 0;
}

/* See the ack_drop note in net.h; one relaxed load per command. */
static bool net_ack_drop(struct udsp *s, const struct net_cmd *cmd)
{
	uint_least32_t armed = atomic_load_explicit(&s->net.ack_drop,
						    memory_order_relaxed);

	if (!armed || armed - 1u != cmd->type)
		return false;
	atomic_store_explicit(&s->net.ack_drop, 0u, memory_order_relaxed);

	return true;
}

static void net_cmds_run(struct udsp *s)
{
	struct net_cmd cmd;

	while (net_cmd_ring_pop(&s->net.cmd, &cmd)) {
		struct udsp_stream *st = stream_at(s, cmd.line, cmd.stream);
		int rc = -EINVAL;

		/* The waiter left, so the work must not run; the same rule the
		 * engine and the slic thread follow. */
		if (cmd_ack_stale(cmd.ack, cmd.token))
			continue;
		if (st && cmd.type == NET_CMD_ATTACH)
			rc = net_cmd_attach(s, st, &cmd);
		else if (st && cmd.type == NET_CMD_DETACH) {
			net_epoll_drop(s, st);
			rc = 0;
		}
		if (net_ack_drop(s, &cmd))
			continue;
		cmd_ack_post(cmd.ack, cmd.token, rc);
	}
}

static void net_fd_drain(int fd)
{
	uint64_t sink;

	while (read(fd, &sink, sizeof(sink)) == (ssize_t)sizeof(sink))
		continue;
}

static void *net_loop(void *arg)
{
	struct udsp *s = arg;

	/* This thread runs SCHED_FIFO (engine_prio - 1) but does not go through
	 * rt_apply, so under MCL_ONFAULT its stack would fault on first use. */
	rt_stack_prefault();

	for (;;) {
		struct epoll_event ev[2 * UDSP_STREAM_MAX * UDSP_LINE_MAX + 1];
		int n;

		net_cmds_run(s);
		if (atomic_load_explicit(&s->net.quit, memory_order_acquire))
			break;

		n = epoll_wait(s->net.epoll_fd, ev, (int)(sizeof(ev) / sizeof(*ev)),
			       -1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		for (int i = 0; i < n; i++) {
			struct net_reg *reg = ev[i].data.ptr;

			if (!reg) {
				net_fd_drain(s->net.cmd_fd);
				continue;
			}
			net_stream_recv(s, reg->stream,
					reg->kind == NET_KIND_RTCP ?
						reg->stream->net_rtcp_fd :
						reg->stream->net_rtp_fd,
					reg->kind);
		}
	}

	return NULL;
}

/* --- control side -------------------------------------------------------- */

int net_init(struct udsp *s)
{
	struct epoll_event ev;

	net_cmd_ring_init(&s->net.cmd);
	atomic_init(&s->net.quit, false);
	atomic_init(&s->net.ack_drop, 0u);
	s->net.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	if (s->net.epoll_fd < 0)
		return -errno;
	s->net.cmd_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (s->net.cmd_fd < 0)
		return -errno;
	if (cmd_ack_init(&s->net.ack))
		return -EMFILE;

	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = NULL;
	if (epoll_ctl(s->net.epoll_fd, EPOLL_CTL_ADD, s->net.cmd_fd, &ev))
		return -errno;

	return 0;
}

int net_start(struct udsp *s)
{
	int prio = s->cfg.engine_prio > 1 ? s->cfg.engine_prio - 1 : 0;
	int rc;

	/* One below the engine and unpinned; prio <= 0 is the test mode and
	 * asks for no real-time setup. */
	rc = udsp_thread_start(&s->net.thread, "udsp-net", net_loop, s,
			       UDSP_NET_STACK_BYTES, -1, -1, prio);
	if (rc)
		return rc;
	s->net.live = true;

	return 0;
}

void net_stop(struct udsp *s)
{
	uint64_t one = 1;

	if (!s->net.live)
		return;

	atomic_store_explicit(&s->net.quit, true, memory_order_release);
	/* The wake can only fail on a counter overflow, and the bounded stop
	 * still has to run: a thread nobody waited for must not be joined
	 * later by the release path. */
	if (write(s->net.cmd_fd, &one, sizeof(one)) != (ssize_t)sizeof(one))
		udsp_op_error(s);
	if (udsp_thread_stop_bounded(&s->net.thread, UDSP_STOP_NET_MS)) {
		atomic_store_explicit(&s->dead, true, memory_order_release);
		return;
	}
	s->net.live = false;
}

void net_release(struct udsp *s)
{
	cmd_ack_release(&s->net.ack);
	if (s->net.cmd_fd >= 0)
		close(s->net.cmd_fd);
	if (s->net.epoll_fd >= 0)
		close(s->net.epoll_fd);
	s->net.cmd_fd = -1;
	s->net.epoll_fd = -1;
}

/* The caller holds ctl_lock, so the single ack slot has exactly one waiter. */
static int net_post(struct udsp *s, const struct net_cmd *in)
{
	struct net_cmd cmd = *in;
	uint64_t one = 1;
	int32_t status = 0;
	uint32_t token;
	int rc;

	if (!s->net.live)
		return -ENOSYS;

	token = ++s->net.token;
	if (!token || token >= CMD_TOKEN_DONE)
		token = s->net.token = 1u;
	cmd.token = token;
	cmd.ack = &s->net.ack;
	cmd_ack_arm(&s->net.ack, token);
	if (!net_cmd_ring_push(&s->net.cmd, &cmd))
		return -ENOSPC;
	if (write(s->net.cmd_fd, &one, sizeof(one)) != (ssize_t)sizeof(one))
		return -EIO;

	rc = cmd_ack_wait(&s->net.ack, token, net_ack_ms_get(), &status);

	return rc ? rc : status;
}

int net_attach(struct udsp *s, unsigned line, unsigned stream, int rtp_fd,
	       int rtcp_fd)
{
	struct net_cmd cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.type = NET_CMD_ATTACH;
	cmd.line = (uint8_t)line;
	cmd.stream = (uint8_t)stream;
	cmd.rtp_fd = rtp_fd;
	cmd.rtcp_fd = rtcp_fd;

	return net_post(s, &cmd);
}

int net_detach(struct udsp *s, unsigned line, unsigned stream)
{
	struct net_cmd cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.type = NET_CMD_DETACH;
	cmd.line = (uint8_t)line;
	cmd.stream = (uint8_t)stream;
	cmd.rtp_fd = -1;
	cmd.rtcp_fd = -1;

	return net_post(s, &cmd);
}

int net_quiesce(struct udsp *s, unsigned line, unsigned stream)
{
	return net_detach(s, line, stream);
}
