// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * A pcapng file writer: a section header block, one interface description
 * block per source, and one enhanced packet block per record.
 *
 * It streams. A block is written as a header, the payload in pieces and a
 * trailer, so the widest record the daemon takes costs no buffer of its own
 * and no size is fixed at compile time. Every write is checked and the first
 * failure closes the file: a capture with a hole in it is worse than a
 * capture that stops and says why.
 *
 * The file is native endian. The byte-order magic of the section header names
 * the endianness it was written with, which is what a reader follows
 * (pcapng, section 4.1), so no field is swapped here.
 *
 * The writer knows nothing of udebug. trace_ring.c feeds it entries and the
 * unit suite reads the file back with a parser of its own.
 */
#ifndef UVOICE_PCAPNG_H
#define UVOICE_PCAPNG_H

#include <stdint.h>
#include <stdio.h>

/* DLT 147 is the first of the private link types, and a string ring exports
 * under it. A reader decides what it means: Wireshark shows the text once the
 * user maps DLT_USER0 to the syslog dissector. The archive README carries the
 * steps. */
#define PCAPNG_LINKTYPE_STRING	147
/* DLT 148 (DLT_USER1) carries the canceller capture: a 64 byte record header
 * and its sample planes (trace_ring.h struct trace_lec_hdr). No dissector
 * knows it; udsp/src/tools/lec-capture.py reads it. */
#define PCAPNG_LINKTYPE_LEC	148

struct pcapng_writer {
	FILE *f;
	uint64_t bytes;		/* what the writer put in the file */
	uint32_t iface_next;	/* the id the next interface block takes */
	/* The first failure and nothing after it: the writer closes the file
	 * on the spot and every later call answers this value, so a caller
	 * that checks once at the end still gets the errno that stopped it. */
	int err;
};

/*
 * Creates the file 0600, truncating an older one, and writes the section
 * header block with app as the application name. 0, or -errno.
 */
int pcapng_open(struct pcapng_writer *w, const char *path, const char *app);

/*
 * One interface description block. name is the interface name option and
 * link_type its link layer. The timestamp resolution is microseconds, which
 * is the unit pcapng_packet_write takes. The id the block takes is stored in
 * id, and it is the value a packet block names. 0, or -errno.
 */
int pcapng_iface_write(struct pcapng_writer *w, uint16_t link_type,
		       const char *name, uint32_t *id);

/*
 * One enhanced packet block of len bytes, stamped stamp_us (microseconds
 * since the epoch, the resolution the interface block declares). The captured
 * and the original length are both len: the caller cuts an entry before it
 * gets here and counts the cut itself. 0, or -errno.
 */
int pcapng_packet_write(struct pcapng_writer *w, uint32_t iface,
			uint64_t stamp_us, const void *data, size_t len);

/*
 * Flushes and closes. bytes, when it is not NULL, takes the file size. 0, or
 * -errno from the flush or the close; the file is closed either way, so the
 * writer is reusable after any answer.
 */
int pcapng_close(struct pcapng_writer *w, uint64_t *bytes);

#endif
