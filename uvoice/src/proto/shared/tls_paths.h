// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The directory of the TLS certificate bundle, the one path that the core,
 * the signaling protocol and the fetch helper share.
 */
#ifndef UVOICE_TLS_PATHS_H
#define UVOICE_TLS_PATHS_H

/*
 * files/uvoice_tls_cert.sh unpacks the fetched archive into this directory
 * (its TLS_DIR), and the script must carry the same path. The environment
 * variable UVOICE_TLS_DIR overrides it in the script and in the bundle check
 * of event.c.
 */
#define UVOICE_TLS_DIR_DEFAULT	"/tmp/voip_tls"

#endif
