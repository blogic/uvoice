// SPDX-License-Identifier: BSD-2-Clause
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * This file stays BSD-2-Clause so a SLIC or fax plug-in and its tools
 * compile against it without LGPL obligations. The rest of libudsp is
 * LGPL-2.1-only.
 *
 * dlopen loader for the SLIC plug-in named in udsp_config.slic_plugin. It
 * resolves exactly one symbol, udsp_slic_descriptor, and validates
 * abi_version, struct_size and the mandatory ops before libudsp calls anything
 * (slic.h). No ProSLIC header is involved, so libudsp stays clear of the
 * Skyworks licence.
 *
 * This file, plugin_load.c, host_linux.[ch], udsp/slic.h and udsp/event.h stay
 * BSD-2-Clause: a SLIC plug-in and its tools compile or
 * link against them. A file added to that interface joins the BSD set.
 */
#ifndef UDSP_SLIC_PLUGIN_LOAD_H
#define UDSP_SLIC_PLUGIN_LOAD_H

#include <stddef.h>

#include "udsp/slic.h"

#define SLIC_PLUGIN_ERR_MAX	256

struct slic_plugin;

/*
 * 0 on success, else a negative errno:
 *   -EINVAL	 no path, or no out pointer
 *   -ENOENT	 dlopen refused the file
 *   -ENOEXEC	 no udsp_slic_descriptor symbol
 *   -EPROTO	 abi_version is not UDSP_SLIC_ABI_VERSION
 *   -EMSGSIZE	 struct_size is not sizeof(struct udsp_slic_ops)
 *   -ENOSYS	 the descriptor omits its name, or an op libudsp always calls
 * err, when given, receives a human readable reason.
 */
int slic_plugin_load(const char *path, struct slic_plugin **out, char *err,
		     size_t errlen);

/*
 * The dlopen half of the loader above, for any descriptor this library
 * resolves by a single symbol. It maps the file with RTLD_NOW | RTLD_LOCAL,
 * resolves symbol, and hands both back; the caller checks the descriptor it
 * asked for and closes the handle on a refusal of its own.
 *
 * 0 on success, else the first three rungs of the ladder above:
 *   -EINVAL	 no path, or no out pointer
 *   -ENOENT	 dlopen refused the file
 *   -ENOEXEC	 the file carries no such symbol
 * err, when given, receives a human readable reason.
 *
 * This function uses dlfcn, stdio and errno only, because the proslic-probe
 * rule compiles this file on its own and links libdl alone.
 */
int plugin_descriptor_load(const char *path, const char *symbol, void **handle,
			   const void **sym, char *err, size_t errlen);

const struct udsp_slic_ops *slic_plugin_ops(const struct slic_plugin *p);
void slic_plugin_unload(struct slic_plugin *p);

#endif
