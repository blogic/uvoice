// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#include <string.h>

#include "cid.h"

/* EN 300 659-1 assigns 0x80 to call-setup MDMF. */
#define CID_MSG_CALL_SETUP 0x80U
/* spandsp adsi.h assigns 0x82 to CLASS and CLIP MDMF message waiting. */
#define CID_MSG_VMWI_MDMF 0x82U
/*
 * The SDMF VMWI message is type 0x06 with three repeated body bytes. GR-30
 * R2-54 leaves the message type to the feature requirements.
 */
#define CID_MSG_VMWI_SDMF 0x06U
/* EN 300 659-1 assigns these MDMF parameter markers. */
#define CID_PARAM_DATETIME 0x01U
#define CID_PARAM_NUMBER 0x02U
#define CID_PARAM_NUMBER_ABSENT 0x04U
#define CID_PARAM_NAME 0x07U
#define CID_PARAM_NAME_ABSENT 0x08U
#define CID_PARAM_VISUAL 0x0bU
/* EN 300 659-1 C-terminated grammar, also documented by spandsp adsi.h. */
#define CID_DTMF_START 'A'
#define CID_DTMF_ABSENCE 'B'
#define CID_DTMF_END 'C'

static size_t value_length_get(const char *value)
{
	return strnlen(value, UDSP_CID_STR_MAX);
}

static bool parameter_add(uint8_t *message, size_t *length, uint8_t type,
			  const void *data, size_t data_length)
{
	if (data_length > UINT8_MAX || *length + data_length + 2U >= UDSP_CID_MSG_MAX)
		return false;
	message[(*length)++] = type;
	message[(*length)++] = (uint8_t)data_length;
	memcpy(message + *length, data, data_length);
	*length += data_length;
	return true;
}

static char absence_reason_get(const struct udsp_cid *cid, bool private_value)
{
	if (private_value)
		return 'P';
	if (cid->blockinfo[0] && !cid->blockinfo[1])
		return cid->blockinfo[0];
	return '\0';
}

static bool identity_add(uint8_t *message, size_t *length, uint8_t value_type,
			 uint8_t reason_type, const char *value, bool private_value,
			 const struct udsp_cid *cid)
{
	size_t value_length = value_length_get(value);
	char reason;

	if (value_length && !private_value)
		return parameter_add(message, length, value_type, value, value_length);
	reason = absence_reason_get(cid, private_value);
	if (!reason)
		return true;
	return parameter_add(message, length, reason_type, &reason, 1);
}

static size_t message_finish(uint8_t *message, size_t length, uint8_t *out,
			     size_t capacity)
{
	unsigned sum = 0;
	size_t i;

	if (length + 1U > capacity || length < 2U || length - 2U > UINT8_MAX)
		return 0;
	message[1] = (uint8_t)(length - 2U);
	for (i = 0; i < length; i++)
		sum += message[i];
	message[length++] = (uint8_t)(0U - sum);
	memcpy(out, message, length);
	return length;
}

static size_t vmwi_sdmf_build(bool on, uint8_t *out, size_t capacity)
{
	uint8_t value = on ? 0x42U : 0x6fU;
	uint8_t message[] = {
		CID_MSG_VMWI_SDMF, 0x03, value, value, value, 0x00
	};
	unsigned sum = message[0] + message[1] + message[2] + message[3] +
		message[4];

	if (capacity < sizeof(message))
		return 0;
	message[5] = (uint8_t)(0U - sum);
	memcpy(out, message, sizeof(message));
	return sizeof(message);
}

static bool format_fsk_is(enum udsp_cid_format format)
{
	return format == UDSP_CIDF_BELLCORE_FSK ||
		format == UDSP_CIDF_ETSI_FSK ||
		format == UDSP_CIDF_ETSI_RPAS ||
		format == UDSP_CIDF_ETSI_DTAS ||
		format == UDSP_CIDF_ETSI_LR_DTAS;
}

size_t udsp_cid_msg_build(const struct udsp_cid *cid,
			  enum udsp_cid_format format, bool type2,
			  uint8_t *out, size_t capacity)
{
	uint8_t message[UDSP_CID_MSG_MAX];
	size_t datetime_length;
	size_t length = 2;

	if (!cid || !out || !format_fsk_is(format))
		return 0;
	(void)type2;
	datetime_length = value_length_get(cid->datetime);
	message[0] = CID_MSG_CALL_SETUP;
	if (datetime_length == 8U &&
	    !parameter_add(message, &length, CID_PARAM_DATETIME,
					       cid->datetime, datetime_length))
		return 0;
	if (!identity_add(message, &length, CID_PARAM_NUMBER,
			  CID_PARAM_NUMBER_ABSENT, cid->number,
			  cid->number_private, cid))
		return 0;
	if (!identity_add(message, &length, CID_PARAM_NAME,
			  CID_PARAM_NAME_ABSENT, cid->name,
			  cid->name_private, cid))
		return 0;
	return message_finish(message, length, out, capacity);
}

size_t udsp_cid_vmwi_build(bool on, const char *datetime,
			   enum udsp_cid_format format, uint8_t *out,
			   size_t capacity)
{
	uint8_t message[UDSP_CID_MSG_MAX];
	uint8_t indicator = on ? 0xffU : 0x00U;
	size_t datetime_length;
	size_t length = 2;

	if (!out)
		return 0;
	if (format == UDSP_CIDF_BELLCORE_FSK)
		return vmwi_sdmf_build(on, out, capacity);
	if (!format_fsk_is(format))
		return 0;
	datetime_length = datetime ? value_length_get(datetime) : 0;
	message[0] = CID_MSG_VMWI_MDMF;
	if (datetime_length == 8U &&
	    !parameter_add(message, &length, CID_PARAM_DATETIME, datetime,
				   datetime_length))
		return 0;
	if (!parameter_add(message, &length, CID_PARAM_VISUAL, &indicator, 1))
		return 0;
	return message_finish(message, length, out, capacity);
}

size_t udsp_cid_dtmf_string(const struct udsp_cid *cid,
			    enum udsp_cid_format format, char *out,
			    size_t capacity)
{
	char message[UDSP_CID_STR_MAX + 4U];
	size_t number_length;
	size_t length = 0;
	char reason;

	if (!cid || !out || (format != UDSP_CIDF_DTMF &&
	    format != UDSP_CIDF_DTMF_DTAS))
		return 0;
	message[length++] = CID_DTMF_START;
	number_length = value_length_get(cid->number);
	if (number_length && !cid->number_private) {
		memcpy(message + length, cid->number, number_length);
		length += number_length;
	} else {
		reason = absence_reason_get(cid, cid->number_private);
		if (!reason)
			return 0;
		message[length++] = CID_DTMF_ABSENCE;
		message[length++] = reason == 'P' ? '1' : '0';
		message[length++] = '0';
	}
	message[length++] = CID_DTMF_END;
	if (capacity <= length)
		return 0;
	memcpy(out, message, length);
	out[length] = '\0';
	return length;
}
