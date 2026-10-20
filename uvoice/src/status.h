// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Shared status renderers. Every entry runs on the uloop thread.
 */
#ifndef UVOICE_STATUS_H
#define UVOICE_STATUS_H

#include <stdint.h>

struct blob_buf;
struct hal_lec_stats;

const char *status_lec_kind_word(uint8_t kind);
const char *status_lec_ec_word(const struct hal_lec_stats *lec);
const char *status_lec_nlp_word(const struct hal_lec_stats *lec);

void status_hal_dump(struct blob_buf *b);
void status_line_rows_dump(struct blob_buf *b, unsigned int line);
void status_streams_dump(struct blob_buf *b);
void status_jb_dump(struct blob_buf *b);
void status_lec_dump(struct blob_buf *b);
void status_monitor_dump(struct blob_buf *b);
/* The backend's diagnostic counters. want_line < 0 renders every line, the
 * accounts_status_dump() convention. */
void status_diag_dump(struct blob_buf *b, int want_line);

#endif
