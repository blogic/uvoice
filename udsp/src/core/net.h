// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * The net thread. It blocks in epoll_wait over the attached stream
 * descriptors plus one command eventfd and does exactly three things per
 * datagram: recvmmsg into a free slot of the stream's SPSC ring, stamp
 * CLOCK_MONOTONIC (SO_TIMESTAMP is wall time and an NTP step would corrupt
 * the jitter estimate), and unprotect SRTP in place on the receive session,
 * which srtp.h reserves for this thread.
 *
 * It parses nothing and keeps no state beyond the descriptor set and that
 * receive session, because rtp_packet_parse mutates the receiver the engine
 * owns.
 *
 * It never logs, never allocates and never takes a mutex: the ring slots are
 * allocated with the stream pool at open and the epoll set is changed only
 * through the command channel below.
 */
#ifndef UDSP_CORE_NET_H
#define UDSP_CORE_NET_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "cmd.h"
#include "spsc.h"
#include "thread.h"

#define UDSP_NET_CMD_DEPTH	8u
/* One tick boundary plus the epoll wake; the same class as a SYNC-TICK ack. */
#define UDSP_NET_ACK_MS		100u
#define UDSP_STOP_NET_MS	500u
#define UDSP_NET_STACK_BYTES	(128 * 1024)
/* One recvmmsg batch. The ring holds 32 slots, so a batch never wraps twice. */
#define UDSP_NET_BATCH		8u

struct udsp;

enum net_cmd_type {
	NET_CMD_NONE = 0,
	NET_CMD_ATTACH,
	NET_CMD_DETACH,
};

struct net_cmd {
	uint8_t type;			/* enum net_cmd_type */
	uint8_t line;
	uint8_t stream;
	int rtp_fd;
	int rtcp_fd;
	uint32_t token;
	struct cmd_ack *ack;
};

UDSP_SPSC_DECLARE(net_cmd_ring, struct net_cmd, UDSP_NET_CMD_DEPTH);

struct udsp_net {
	struct udsp_thread thread;
	int epoll_fd;
	int cmd_fd;
	struct net_cmd_ring cmd;
	struct cmd_ack ack;
	uint32_t token;			/* control thread, under ctl_lock */
	atomic_bool quit;
	bool live;
	/*
	 * Test-only fault injection. net_cmd_attach installs the epoll entries
	 * before its ack, so the defect lives in the window where the work ran
	 * and the ack did not arrive. A test arms the type it wants lost, as
	 * enum net_cmd_type plus one, and the net thread swallows that one
	 * ack; zero, the value of every shipped run, disarms it.
	 */
	_Atomic uint_least32_t ack_drop;
};

unsigned net_ack_ms_get(void);

/* Both run on the control thread. init leaves the thread stopped so a failed
 * open can release the descriptors without a join. */
int net_init(struct udsp *s);
int net_start(struct udsp *s);
void net_stop(struct udsp *s);
void net_release(struct udsp *s);

/* Hand the net thread one stream's descriptors. libudsp dups them; the caller
 * may close its originals after the reconfig ack returns. */
int net_attach(struct udsp *s, unsigned line, unsigned stream, int rtp_fd,
	       int rtcp_fd);
/* Remove the stream from the epoll set and drop libudsp's dups before
 * returning, so the backend closes its originals with no fd-reuse race. */
int net_detach(struct udsp *s, unsigned line, unsigned stream);
/* Wait for in-flight I/O on one stream to finish; the tick-boundary half of
 * the running-stream reconfiguration of udsp.h section 3. The net thread does
 * the epoll removal itself and acks after it, so nothing is in flight once
 * this returns. */
int net_quiesce(struct udsp *s, unsigned line, unsigned stream);

#endif
