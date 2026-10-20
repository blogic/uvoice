// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * The T.38 gateway plug-in named in udsp_config.fax_plugin. This file loads
 * it, validates the descriptor of udsp/fax.h and holds the mapping.
 *
 * Rules.
 * 1. The control thread loads the plug-in in udsp_open and unloads it in
 *    udsp_release, after stream_pool_shutdown has torn every stream down. A
 *    gateway instance is destroyed in that teardown, so the code that runs
 *    destroy must still be mapped when it does.
 * 2. A load that fails does not fail udsp_open. The library counts one
 *    op_error and leaves UDSP_FEAT_T38 and the UDSP_SEL_T38 codec bit clear,
 *    so a stale file costs the fax upgrade and no voice line. The two bits
 *    are set together: the backend offers T.38 on the feature bit and accepts
 *    the answer on the codec bit.
 * 3. The control thread calls create and destroy, under ctl_lock and with the
 *    stream quiesced. The one exception is the teardown of rule 1: it calls
 *    destroy without ctl_lock, after every other thread stopped. The engine
 *    tick calls rx_pcm, tx_pcm, udptl_in and
 *    udptl_out. No other thread calls an op, so the plug-in needs no lock of
 *    its own.
 * 4. The gateway works at 8000 Hz on every line rate (udsp/fax.h: one
 *    80-sample frame each way). The stream layer converts the rate with its
 *    own resamplers, so the per-line detector feeds of core/chan.h carry
 *    nothing for it.
 */
#ifndef UDSP_CORE_FAX_PLUGIN_H
#define UDSP_CORE_FAX_PLUGIN_H

#include <stddef.h>

#include "udsp/fax.h"

#define FAX_PLUGIN_ERR_MAX	256

struct fax_plugin;

/*
 * 0 on success, else a negative errno:
 *   -EINVAL	 no path, or no out pointer
 *   -ENOENT	 dlopen refused the file
 *   -ENOEXEC	 no udsp_fax_descriptor symbol
 *   -EPROTO	 abi_version is not UDSP_FAX_ABI_VERSION
 *   -EMSGSIZE	 struct_size is not sizeof(struct udsp_fax_ops)
 *   -ENOSYS	 the descriptor omits its name or one of the six ops
 *   -ENOMEM	 no memory for the handle
 * err, when given, receives a human readable reason.
 */
int fax_plugin_load(const char *path, struct fax_plugin **out, char *err,
		    size_t errlen);

const struct udsp_fax_ops *fax_plugin_ops(const struct fax_plugin *p);
void fax_plugin_unload(struct fax_plugin *p);

#endif
