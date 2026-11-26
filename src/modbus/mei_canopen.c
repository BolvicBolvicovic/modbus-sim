/**
 * @file
 * @brief FC 43 MEI 0x0D (CANopen General Reference) SDO upload and download.
 */
#include "modbus/mei_canopen.h"
#include "modbus/bytes.h"
#include "modbus/pdu.h"
#include <string.h>

enum canopen_extended_exception
{
	CANOPEN_EXT_UNSUPPORTED_OPTION = 0xAE,
	CANOPEN_EXT_SERVICE_FAILURE    = 0xCE,
};

static u8
canopen_element_size(u8 data_type)
{
	switch (data_type)
	{
	case 0x02: /* Unsigned24 */
		return 3;
	case 0x01: /* Unsigned16 */
	case 0x11: /* Integer16 */
		return 2;
	case 0x03: /* Unsigned32 */
	case 0x13: /* Integer32 */
	case 0x20: /* Real32 */
		return 4;
	case 0x21: /* Real64 */
		return 8;
	case 0x00: /* Unsigned8 */
	case 0x50: /* Visible string */
	case 0x60: /* Domain */
	default:
		return 1;
	}
}

static u32
canopen_data_len(const canopen_request_t* req)
{
	if (!req->write)
		return 0;
	if (req->has_data_type)
		return (u32)req->count * canopen_element_size(req->data_type);
	return req->count;
}

static bool
canopen_layout(const u8* pdu, u32 pdu_len, canopen_request_t* out)
{
	u32 off = 3; /* index of the reserved byte */

	memset(out, 0, sizeof(*out));
	if (pdu_len < 4 ||
	    pdu[0] != FC_ENCAPSULATED_INTERFACE_TRANSPORT ||
	    pdu[1] != MEI_TYPE_CANOPEN)
		return false;

	out->control = pdu[2];
	if (out->control & CANOPEN_CTRL_EXTENDED_CTRL)
		off++;
	if (pdu_len <= off)
		return false;
	out->reserved = pdu[off++];

	if (out->control & CANOPEN_CTRL_COUNTER)
	{
		if (pdu_len <= off)
			return false;
		off++;
	}
	if (out->control & CANOPEN_CTRL_NETWORK_ID)
	{
		if (pdu_len <= off)
			return false;
		out->has_network_id = true;
		out->network_id     = pdu[off++];
	}
	if (out->control & CANOPEN_CTRL_ENCODED_DATA)
	{
		if (pdu_len <= off)
			return false;
		out->has_data_type = true;
		out->data_type     = pdu[off++];
	}
	if (pdu_len < off + 8)
		return false;

	out->write    = (out->control & CANOPEN_CTRL_ACCESS) != 0;
	out->node     = pdu[off];
	out->index    = get_be16(pdu + off + 1);
	out->subindex = pdu[off + 3];
	out->start    = get_be16(pdu + off + 4);
	out->count    = get_be16(pdu + off + 6);
	out->data_offset = off + 8;
	out->data_len    = canopen_data_len(out);
	return true;
}

bool
canopen_expected_pdu_len(const u8* pdu, u32 pdu_len, u32* expected)
{
	canopen_request_t req;

	if (!canopen_layout(pdu, pdu_len, &req))
		return false;

	*expected = req.data_offset + req.data_len;
	return true;
}

bool
canopen_parse_header(const u8* pdu, u32 pdu_len, canopen_request_t* out)
{
	return canopen_layout(pdu, pdu_len, out);
}

bool
canopen_parse_request(const u8* pdu, u32 pdu_len, canopen_request_t* out)
{
	u32 expected = 0;

	if (!canopen_expected_pdu_len(pdu, pdu_len, &expected) ||
	    expected != pdu_len)
		return false;
	return canopen_layout(pdu, pdu_len, out);
}

u32
canopen_unsupported_option_exception(u8* resp, u8 supported_control)
{
	resp[0] = (u8)(FC_ENCAPSULATED_INTERFACE_TRANSPORT | 0x80);
	resp[1] = 0xFF;
	resp[2] = 0x00;
	resp[3] = 0x03;
	resp[4] = MEI_TYPE_CANOPEN;
	resp[5] = CANOPEN_EXT_UNSUPPORTED_OPTION;
	resp[6] = supported_control;
	return 7;
}

u32
canopen_service_exception(u8* resp, u32 error_code)
{
	resp[0] = (u8)(FC_ENCAPSULATED_INTERFACE_TRANSPORT | 0x80);
	resp[1] = 0xFF;
	resp[2] = 0x00;
	resp[3] = 0x06;
	resp[4] = MEI_TYPE_CANOPEN;
	resp[5] = CANOPEN_EXT_SERVICE_FAILURE;
	resp[6] = (u8)(error_code & 0xFF);
	resp[7] = (u8)((error_code >> 8) & 0xFF);
	resp[8] = (u8)((error_code >> 16) & 0xFF);
	resp[9] = (u8)((error_code >> 24) & 0xFF);
	return 10;
}
