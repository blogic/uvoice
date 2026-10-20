// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * The test-only T.38 gateway plug-in: the fake of test/fax_fake.h behind the
 * descriptor udsp/fax.h names, built as a shared object so the loader ladder
 * and the unload order run through a real dlopen. The build never installs
 * it, and test_media reaches it by the path the build passes in.
 */
#include "fax_fake.h"

const struct udsp_fax_descriptor udsp_fax_descriptor = {
	.abi_version = UDSP_FAX_ABI_VERSION,
	.struct_size = sizeof(struct udsp_fax_ops),
	.ops = &fax_fake_ops,
};
