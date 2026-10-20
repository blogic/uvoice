// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "../slic/plugin_load.h"

#include "fax_plugin.h"

#define FAX_PLUGIN_SYMBOL	"udsp_fax_descriptor"

struct fax_plugin {
	void *handle;
	const struct udsp_fax_ops *ops;
};

/*
 * The reason text is formatted here and not by the shared dlopen helper of
 * slic/plugin_load.c: that file compiles on its own for proslic-probe and
 * stays BSD-2-Clause, so its formatter is not part of the interface the
 * ProSLIC plug-in links against.
 */
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

/* udsp/fax.h declares name as a plain const char * and six ops the library
 * calls on every session, so a descriptor without them is refused before any
 * stream can reach one. */
static int ops_validate(const struct udsp_fax_ops *ops, char *err,
			size_t errlen)
{
	if (!ops) {
		err_set(err, errlen, "descriptor carries no ops");
		return -ENOSYS;
	}

	if (!ops->name || !*ops->name) {
		err_set(err, errlen, "plug-in reports no gateway name");
		return -ENOSYS;
	}

	if (!ops->create || !ops->destroy || !ops->rx_pcm || !ops->tx_pcm ||
	    !ops->udptl_in || !ops->udptl_out) {
		err_set(err, errlen, "plug-in %s omits a mandatory op",
			ops->name);
		return -ENOSYS;
	}

	return 0;
}

int fax_plugin_load(const char *path, struct fax_plugin **out, char *err,
		    size_t errlen)
{
	const struct udsp_fax_descriptor *desc;
	struct fax_plugin *p;
	const void *sym;
	void *handle;
	int rc;

	if (err && errlen)
		err[0] = '\0';

	if (!out) {
		err_set(err, errlen, "no plug-in path");
		return -EINVAL;
	}

	rc = plugin_descriptor_load(path, FAX_PLUGIN_SYMBOL, &handle, &sym, err,
				    errlen);
	if (rc)
		return rc;

	desc = sym;
	if (desc->abi_version != UDSP_FAX_ABI_VERSION) {
		err_set(err, errlen, "%s: abi %u, expected %u", path,
			desc->abi_version, (unsigned)UDSP_FAX_ABI_VERSION);
		dlclose(handle);
		return -EPROTO;
	}

	if (desc->struct_size != sizeof(struct udsp_fax_ops)) {
		err_set(err, errlen, "%s: ops size %u, expected %zu", path,
			desc->struct_size, sizeof(struct udsp_fax_ops));
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

const struct udsp_fax_ops *fax_plugin_ops(const struct fax_plugin *p)
{
	return p ? p->ops : NULL;
}

void fax_plugin_unload(struct fax_plugin *p)
{
	if (!p)
		return;
	if (p->handle)
		dlclose(p->handle);
	free(p);
}
