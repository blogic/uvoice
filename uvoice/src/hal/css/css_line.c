// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 */
#include "css_int.h"

enum hal_status css_line_state_set(struct hal *h, unsigned int line,
				   enum hal_line_state state)
{
	return HAL_OK;
}

enum hal_status css_line_polarity_set(struct hal *h, unsigned int line,
				      enum hal_polarity pol)
{
	return HAL_UNSUPPORTED;
}

enum hal_status css_line_hook_timing_set(struct hal *h, unsigned int line,
					 const struct hal_hook_timing *ts)
{
	return HAL_OK;
}

enum hal_status css_line_cid_send(struct hal *h, unsigned int line,
				  enum hal_cid_kind kind,
				  const struct hal_cid *cid)
{
	return HAL_UNSUPPORTED;
}

enum hal_status css_line_mwi_set(struct hal *h, unsigned int line,
				 enum hal_mwi mwi)
{
	return css_led_set(h, HAL_LED_MESSAGE,
			   mwi == HAL_MWI_ON ? HAL_LED_BLINK_SLOW : HAL_LED_OFF);
}

enum hal_status css_line_test_start(struct hal *h, unsigned int line,
				    enum hal_line_test test,
				    struct hal_job *job)
{
	return HAL_UNSUPPORTED;
}

enum hal_status css_line_test_result_get(struct hal *h, unsigned int line,
					 void *buf, size_t buf_len,
					 size_t *out_len)
{
	return HAL_UNSUPPORTED;
}

enum hal_status css_channel_config_set(struct hal *h, unsigned int line,
				       const struct hal_channel_cfg *cfg)
{
	return HAL_OK;
}

enum hal_status css_debug_set(struct hal *h, uint32_t flags)
{
	return HAL_UNSUPPORTED;
}
