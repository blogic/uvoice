// SPDX-License-Identifier: BSD-2-Clause
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * This file stays BSD-2-Clause so a SLIC or fax plug-in and its tools
 * compile against it without LGPL obligations. The rest of libudsp is
 * LGPL-2.1-only.
 */
#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "plugin_load.h"

#define PLUGIN_SYMBOL	"udsp_slic_descriptor"

struct slic_plugin {
	void *handle;
	const struct udsp_slic_ops *ops;
};

static void err_set(char *err, size_t errlen, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

static void err_set(char *err, size_t errlen, const char *fmt, ...)
{
	va_list ap;

	if (!err || errlen == 0)
		return;

	va_start(ap, fmt);
	vsnprintf(err, errlen, fmt, ap);
	va_end(ap);
}

static int ops_validate(const struct udsp_slic_ops *ops, char *err, size_t errlen)
{
	if (!ops) {
		err_set(err, errlen, "descriptor carries no ops");
		return -ENOSYS;
	}

	/* slic.h declares name as a plain const char *, not a nullable op, and
	 * udsp_cap.backend is filled from it. */
	if (!ops->name || !*ops->name) {
		err_set(err, errlen, "plug-in reports no backend name");
		return -ENOSYS;
	}

	if (!ops->attach || !ops->detach || !ops->init || !ops->linefeed_set ||
	    !ops->ring_start || !ops->ring_stop || !ops->hook_read ||
	    !ops->fault_read || !ops->gain_set || !ops->safe_xfer) {
		err_set(err, errlen, "plug-in %s omits a mandatory op", ops->name);
		return -ENOSYS;
	}

	return 0;
}

int plugin_descriptor_load(const char *path, const char *symbol, void **handle,
			   const void **sym, char *err, size_t errlen)
{
	void *h;
	void *s;

	if (err && errlen)
		err[0] = '\0';

	if (!path || !*path || !symbol || !handle || !sym) {
		err_set(err, errlen, "no plug-in path");
		return -EINVAL;
	}

	/* RTLD_LOCAL keeps the proprietary symbols out of the global namespace,
	 * so a second plug-in cannot bind to this one's ProSLIC copy. */
	h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!h) {
		err_set(err, errlen, "dlopen %s: %s", path, dlerror());
		return -ENOENT;
	}

	dlerror();
	s = dlsym(h, symbol);
	if (!s) {
		err_set(err, errlen, "%s: no %s symbol", path, symbol);
		dlclose(h);
		return -ENOEXEC;
	}

	*handle = h;
	*sym = s;

	return 0;
}

int slic_plugin_load(const char *path, struct slic_plugin **out, char *err,
		     size_t errlen)
{
	const struct udsp_slic_descriptor *desc;
	struct slic_plugin *p;
	const void *sym;
	void *handle;
	int rc;

	if (err && errlen)
		err[0] = '\0';

	if (!out) {
		err_set(err, errlen, "no plug-in path");
		return -EINVAL;
	}

	rc = plugin_descriptor_load(path, PLUGIN_SYMBOL, &handle, &sym, err,
				    errlen);
	if (rc)
		return rc;

	desc = sym;
	if (desc->abi_version != UDSP_SLIC_ABI_VERSION) {
		err_set(err, errlen, "%s: abi %u, expected %u", path,
			desc->abi_version, (unsigned)UDSP_SLIC_ABI_VERSION);
		dlclose(handle);
		return -EPROTO;
	}

	if (desc->struct_size != sizeof(struct udsp_slic_ops)) {
		err_set(err, errlen, "%s: ops size %u, expected %zu", path,
			desc->struct_size, sizeof(struct udsp_slic_ops));
		dlclose(handle);
		return -EMSGSIZE;
	}

	rc = ops_validate(desc->ops, err, errlen);
	if (rc) {
		dlclose(handle);
		return rc;
	}

	p = calloc(1, sizeof(*p));
	if (!p) {
		err_set(err, errlen, "out of memory");
		dlclose(handle);
		return -ENOMEM;
	}

	p->handle = handle;
	p->ops = desc->ops;
	*out = p;
	return 0;
}

const struct udsp_slic_ops *slic_plugin_ops(const struct slic_plugin *p)
{
	return p ? p->ops : NULL;
}

void slic_plugin_unload(struct slic_plugin *p)
{
	if (!p)
		return;
	if (p->handle)
		dlclose(p->handle);
	free(p);
}
