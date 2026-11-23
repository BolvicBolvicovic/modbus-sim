#include "modbus/pdu.h"
#include "modbus/bytes.h"
#include "modbus/mbap.h"
#include "vlib/macros.h"
#include <string.h>

_Static_assert(MODBUS_MAX_PDU / 2 == DEVICE_MAX_RECORD_VALUES,
	       "file record value limit must match Modbus PDU capacity");
_Static_assert(4 + DEVICE_SERVER_ID_ADDITIONAL_DATA_MAX == MODBUS_MAX_PDU,
	       "FC 17 response must fit a Modbus PDU");

u32
modbus_exception(u8* resp, u8 function, u8 code)
{
	resp[0] = function | 0x80;
	resp[1] = code;
	return 2;
}

/* --- reads --- */

static u32
read_bits(device_t* dev, space_kind_t space, const u8* req, u32 req_len, u8* resp)
{
	u16	bits[MB_MAX_READ_BITS];
	u32	address = 0;
	u32	qty     = 0;
	u32	bytes   = 0;

	if (req_len != 5)
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

	address = get_be16(req + 1);
	qty     = get_be16(req + 3);
	if (qty < 1 || qty > MB_MAX_READ_BITS)
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

	if (!device_read(dev, space, address, qty, bits))
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_ADDRESS);

	bytes   = (qty + 7) / 8;
	resp[0] = req[0];
	resp[1] = (u8)bytes;
	memset(resp + 2, 0, bytes);
	for (u32 i = 0; i < qty; i++)
		resp[2 + i / 8] |= (u8)((bits[i] ? 1 : 0) << (i % 8));

	return 2 + bytes;
}

static u32
read_registers(device_t* dev, space_kind_t space, const u8* req, u32 req_len, u8* resp)
{
	u16	words[MB_MAX_READ_REGISTERS];
	u32	address = 0;
	u32	qty     = 0;

	if (req_len != 5)
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

	address = get_be16(req + 1);
	qty     = get_be16(req + 3);
	if (qty < 1 || qty > MB_MAX_READ_REGISTERS)
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

	if (!device_read(dev, space, address, qty, words))
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_ADDRESS);

	resp[0] = req[0];
	resp[1] = (u8)(qty * 2);
	for (u32 i = 0; i < qty; i++)
		put_be16(resp + 2 + i * 2, words[i]);

	return 2 + qty * 2;
}

/* --- writes --- */

static u32
write_single_coil(device_t* dev, const u8* req, u32 req_len, u8* resp)
{
	u32	address = 0;
	u16	raw     = 0;
	u16	bit     = 0;

	if (req_len != 5)
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

	address = get_be16(req + 1);
	raw     = get_be16(req + 3);
	if (raw != 0xFF00 && raw != 0x0000)
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

	bit = (raw == 0xFF00);
	if (!device_write(dev, SPACE_COIL, address, 1, &bit))
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_ADDRESS);

	memcpy(resp, req, 5);
	return 5;
}

static u32
write_single_register(device_t* dev, const u8* req, u32 req_len, u8* resp)
{
	u32	address = 0;
	u16	value   = 0;

	if (req_len != 5)
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

	address = get_be16(req + 1);
	value   = get_be16(req + 3);
	if (!device_write(dev, SPACE_HOLDING, address, 1, &value))
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_ADDRESS);

	memcpy(resp, req, 5);
	return 5;
}

static u32
write_multiple_coils(device_t* dev, const u8* req, u32 req_len, u8* resp)
{
	u16	bits[MB_MAX_WRITE_BITS];
	u32	address = 0;
	u32	qty     = 0;

	if (req_len < 6)
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

	address = get_be16(req + 1);
	qty     = get_be16(req + 3);
	if (qty < 1 || qty > MB_MAX_WRITE_BITS)
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

	if (req[5] != (qty + 7) / 8 || req_len != 6 + (u32)req[5])
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

	for (u32 i = 0; i < qty; i++)
		bits[i] = (req[6 + i / 8] >> (i % 8)) & 1;

	if (!device_write(dev, SPACE_COIL, address, qty, bits))
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_ADDRESS);

	memcpy(resp, req, 5);
	return 5;
}

static u32
write_multiple_registers(device_t* dev, const u8* req, u32 req_len, u8* resp)
{
	u16	words[MB_MAX_WRITE_REGISTERS];
	u32	address = 0;
	u32	qty     = 0;

	if (req_len < 6)
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

	address = get_be16(req + 1);
	qty     = get_be16(req + 3);
	if (qty < 1 || qty > MB_MAX_WRITE_REGISTERS)
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

	if (req[5] != qty * 2 || req_len != 6 + (u32)req[5])
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

	for (u32 i = 0; i < qty; i++)
		words[i] = get_be16(req + 6 + i * 2);

	if (!device_write(dev, SPACE_HOLDING, address, qty, words))
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_ADDRESS);

	memcpy(resp, req, 5);
	return 5;
}

static u32
read_write_multiple_registers(device_t* dev, const u8* req, u32 req_len, u8* resp)
{
	u16	write_words[121];
	u16	read_words[MB_MAX_READ_REGISTERS];
	u32	read_addr   = 0;
	u32	read_qty    = 0;
	u32	write_addr  = 0;
	u32	write_qty   = 0;
	u32	byte_count  = 0;

	if (req_len < 10)
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

	read_addr  = get_be16(req + 1);
	read_qty   = get_be16(req + 3);
	write_addr = get_be16(req + 5);
	write_qty  = get_be16(req + 7);
	byte_count = req[9];

	if (read_qty < 1 ||
	    read_qty > 0x007D ||
	    write_qty < 1 ||
	    write_qty > 0x0079)
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

	if (byte_count != write_qty * 2 || req_len != 10 + byte_count)
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

	for (u32 i = 0; i < write_qty; i++)
		write_words[i] = get_be16(req + 10 + i * 2);

	/* All-or-nothing: validate availability of both ranges before applying any write. */
	if (!device_read(dev, SPACE_HOLDING, write_addr, write_qty, read_words))
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_ADDRESS);
	if (!device_read(dev, SPACE_HOLDING, read_addr, read_qty, read_words))
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_ADDRESS);

	if (!device_write(dev, SPACE_HOLDING, write_addr, write_qty, write_words))
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_ADDRESS);

	/* Write happens before read; result reflects post-write values (including overlap). */
	if (!device_read(dev, SPACE_HOLDING, read_addr, read_qty, read_words))
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_ADDRESS);

	resp[0] = req[0];
	resp[1] = (u8)(read_qty * 2);
	for (u32 i = 0; i < read_qty; i++)
		put_be16(resp + 2 + i * 2, read_words[i]);

	return 2 + read_qty * 2;
}

static u32
read_fifo_queue(device_t* dev, const u8* req, u32 req_len, u8* resp)
{
	u16	values[DEVICE_MAX_FIFO_VALUES];
	u16	count   = 0;
	u16	address = 0;

	if (req_len != 3)
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

	address = get_be16(req + 1);
	if (!device_read_fifo(dev, address, &count, values))
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_ADDRESS);

	resp[0] = req[0];
	put_be16(resp + 1, (u16)(2 + 2 * (u32)count));
	put_be16(resp + 3, count);
	for (u32 i = 0; i < count; i++)
		put_be16(resp + 5 + i * 2, values[i]);

	return 5 + 2 * (u32)count;
}

static bool
read_file_record_shape_ok(const u8* req, u32 req_len, u32* subreq_count, u32* resp_data_len)
{
	u32	byte_count = 0;

	if (req_len < 2)
		return false;

	byte_count = req[1];
	if (byte_count < 0x07 ||
	    byte_count > 0xF5 ||
	    (byte_count % 7) != 0)
		return false;
	if (req_len != 2 + byte_count)
		return false;

	*subreq_count  = byte_count / 7;
	*resp_data_len = 0;
	for (u32 i = 0; i < *subreq_count; i++)
	{
		const u8*	sr      = req + 2 + i * 7;
		u32		file_no = get_be16(sr + 1);
		u32		rec_no  = get_be16(sr + 3);
		u32		rec_len = get_be16(sr + 5);

		if (sr[0] != 0x06 ||
		    file_no < 0x0001 ||
		    rec_no > 0x270F ||
		    rec_len < 1 ||
		    rec_no + rec_len - 1 > 0x270F)
			return false;
		*resp_data_len += 2 + rec_len * 2;
	}

	if (2 + *resp_data_len > MODBUS_MAX_PDU)
		return false;

	return true;
}

static bool
read_file_record_available(device_t* dev, const u8* req, u32 subreq_count)
{
	for (u32 i = 0; i < subreq_count; i++)
	{
		u16		words[MODBUS_MAX_PDU / 2];
		const u8*	sr      = req + 2 + i * 7;
		u16		file_no = get_be16(sr + 1);
		u16		rec_no  = get_be16(sr + 3);
		u16		rec_len = get_be16(sr + 5);

		if (!device_read_file_records(dev, file_no, rec_no, rec_len, words))
			return false;
	}
	return true;
}

static u32
read_file_record_build(device_t* dev, const u8* req, u32 subreq_count, u32 resp_data_len, u8* resp)
{
	u32	offset = 2;

	resp[0] = req[0];
	resp[1] = (u8)resp_data_len;
	for (u32 i = 0; i < subreq_count; i++)
	{
		u16		words[MODBUS_MAX_PDU / 2];
		const u8*	sr      = req + 2 + i * 7;
		u16		file_no = get_be16(sr + 1);
		u16		rec_no  = get_be16(sr + 3);
		u16		rec_len = get_be16(sr + 5);

		/* Availability was validated above. */
		device_read_file_records(dev, file_no, rec_no, rec_len, words);

		resp[offset++] = (u8)(1 + rec_len * 2);
		resp[offset++] = 0x06;
		for (u32 j = 0; j < rec_len; j++)
		{
			put_be16(resp + offset, words[j]);
			offset += 2;
		}
	}
	return 2 + resp_data_len;
}

static u32
read_file_record(device_t* dev, const u8* req, u32 req_len, u8* resp)
{
	u32	subreq_count  = 0;
	u32	resp_data_len = 0;

	if (!read_file_record_shape_ok(req, req_len, &subreq_count, &resp_data_len))
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

	if (!read_file_record_available(dev, req, subreq_count))
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_ADDRESS);

	return read_file_record_build(dev, req, subreq_count, resp_data_len, resp);
}

static bool
write_file_record_shape_ok(const u8* req, u32 req_len, u32* byte_count)
{
	u32	offset = 0;

	if (req_len < 2)
		return false;

	*byte_count = req[1];
	if (*byte_count < 0x09 || *byte_count > 0xFB)
		return false;
	if (req_len != 2 + *byte_count)
		return false;

	offset = 2;
	while (offset < 2 + *byte_count)
	{
		const u8*	sr      = req + offset;
		u32		remain  = 2 + *byte_count - offset;
		u32		file_no = 0;
		u32		rec_no  = 0;
		u32		rec_len = 0;
		u32		sr_len  = 0;

		if (remain < 7)
			return false;

		file_no = get_be16(sr + 1);
		rec_no  = get_be16(sr + 3);
		rec_len = get_be16(sr + 5);
		sr_len  = 7 + rec_len * 2;

		if (sr[0] != 0x06 || file_no < 0x0001)
			return false;
		if (rec_no > 0x270F || rec_no + rec_len - 1 > 0x270F)
			return false;
		if (rec_len < 1 || sr_len > remain)
			return false;

		offset += sr_len;
	}
	return true;
}

static bool
write_file_record_can_write(device_t* dev, const u8* req, u32 byte_count)
{
	u32	offset = 2;

	while (offset < 2 + byte_count)
	{
		const u8*	sr      = req + offset;
		u16		file_no = get_be16(sr + 1);
		u16		rec_no  = get_be16(sr + 3);
		u16		rec_len = get_be16(sr + 5);

		if (!device_can_write_file_records(dev, file_no, rec_no, rec_len))
			return false;

		offset += 7 + (u32)rec_len * 2;
	}
	return true;
}

static bool
write_file_record_apply(device_t* dev, const u8* req, u32 byte_count)
{
	u32	offset = 2;

	while (offset < 2 + byte_count)
	{
		const u8*	sr      = req + offset;
		u16		file_no = get_be16(sr + 1);
		u16		rec_no  = get_be16(sr + 3);
		u16		rec_len = get_be16(sr + 5);
		u16		values[MODBUS_MAX_PDU / 2];

		for (u32 i = 0; i < rec_len; i++)
			values[i] = get_be16(sr + 7 + i * 2);

		if (!device_write_file_records(dev, file_no, rec_no, rec_len, values))
			return false;
		offset += 7 + (u32)rec_len * 2;
	}
	return true;
}

static u32
write_file_record(device_t* dev, const u8* req, u32 req_len, u8* resp)
{
	u32	byte_count = 0;

	if (!write_file_record_shape_ok(req, req_len, &byte_count))
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

	if (!write_file_record_can_write(dev, req, byte_count))
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_ADDRESS);

	/* All-or-nothing validated above; return value guards future read-only records. */
	if (!write_file_record_apply(dev, req, byte_count))
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_ADDRESS);

	memcpy(resp, req, req_len);
	return req_len;
}

static u32
mask_write_register(device_t* dev, const u8* req, u32 req_len, u8* resp)
{
	u32	address  = 0;
	u16	and_mask = 0;
	u16	or_mask  = 0;
	u16	current  = 0;
	u16	result   = 0;

	if (req_len != 7)
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

	address  = get_be16(req + 1);
	and_mask = get_be16(req + 3);
	or_mask  = get_be16(req + 5);

	if (!device_read(dev, SPACE_HOLDING, address, 1, &current))
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_ADDRESS);

	result = (u16)((current & and_mask) | (or_mask & (u16)~and_mask));
	if (!device_write(dev, SPACE_HOLDING, address, 1, &result))
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_ADDRESS);

	memcpy(resp, req, 7);
	return 7;
}

static u32
read_exception_status(device_t* dev, const u8* req, u32 req_len, u8* resp)
{
	if (req_len != 1)
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

	resp[0] = req[0];
	resp[1] = dev->exception_status;
	return 2;
}

static bool
diagnostic_subfunction_supported(u16 subfunction)
{
	switch (subfunction)
	{
	case DIAG_SUB_RETURN_QUERY_DATA:
	case DIAG_SUB_RESTART_COMMUNICATIONS:
	case DIAG_SUB_READ_DIAGNOSTIC_REGISTER:
	case DIAG_SUB_CHANGE_ASCII_DELIMITER:
	case DIAG_SUB_LISTEN_ONLY_MODE:
	case DIAG_SUB_CLEAR_COUNTERS:
	case DIAG_SUB_RETURN_BUS_MESSAGE_COUNT:
	case DIAG_SUB_RETURN_BUS_COMM_ERROR_COUNT:
	case DIAG_SUB_RETURN_BUS_EXCEPTION_ERROR_COUNT:
	case DIAG_SUB_RETURN_SLAVE_MESSAGE_COUNT:
	case DIAG_SUB_RETURN_SLAVE_NO_RESPONSE_COUNT:
	case DIAG_SUB_RETURN_SLAVE_NAK_COUNT:
	case DIAG_SUB_RETURN_SLAVE_BUSY_COUNT:
	case DIAG_SUB_RETURN_BUS_CHAR_OVERRUN_COUNT:
	case DIAG_SUB_CLEAR_OVERRUN_COUNTER_AND_FLAG:
		return true;
	default:
		return false;
	}
}

static u32
diagnostic_counter_response(u8* resp, u16 subfunction, u16 value)
{
	resp[0] = FC_DIAGNOSTICS;
	put_be16(resp + 1, subfunction);
	put_be16(resp + 3, value);
	return 5;
}

/* Shared reset for 0x0001 and 0x000A; the ASCII delimiter and listen-only
 * state are deliberately not part of it. */
static void
clear_diagnostic_counters(device_t* dev)
{
	dev->message_count          = 0;
	dev->diagnostic_register    = 0;
	dev->diag_comm_error_count  = 0;
	dev->diag_exception_count   = 0;
	dev->diag_no_response_count = 0;
	dev->diag_nak_count         = 0;
	dev->diag_busy_count        = 0;
	dev->diag_overrun_count     = 0;
}

static u32
diagnostics(device_t* dev, const u8* req, u32 req_len, u8* resp)
{
	u16	subfunction      = 0;
	u16	data            = 0;
	bool	was_listen_only = false;

	if (req_len < 3)
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

	subfunction = get_be16(req + 1);
	/* Return Query Data keeps its original N x 2-byte echo shape. */
	if (subfunction == DIAG_SUB_RETURN_QUERY_DATA)
	{
		if (req_len < 5 || (req_len % 2) == 0)
			return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

		memcpy(resp, req, req_len);
		return req_len;
	}
	if (!diagnostic_subfunction_supported(subfunction))
		return modbus_exception(resp, req[0], EXC_ILLEGAL_FUNCTION);
	if (req_len != 5)
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

	data = get_be16(req + 3);
	if (subfunction != DIAG_SUB_RESTART_COMMUNICATIONS &&
	    subfunction != DIAG_SUB_CHANGE_ASCII_DELIMITER &&
	    data != 0)
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

	switch (subfunction)
	{
	case DIAG_SUB_RESTART_COMMUNICATIONS:
		if (data != 0x0000 && data != 0xFF00)
			return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

		was_listen_only      = dev->listen_only;
		clear_diagnostic_counters(dev);
		if (data == 0xFF00)
			dev->com_event_count = 0;
		dev->listen_only = false;
		/* Restart is processed in listen-only mode, but never answered there. */
		if (was_listen_only)
			return 0;

		memcpy(resp, req, 5);
		return 5;
	case DIAG_SUB_READ_DIAGNOSTIC_REGISTER:
		return diagnostic_counter_response(resp, subfunction, dev->diagnostic_register);
	case DIAG_SUB_CHANGE_ASCII_DELIMITER:
		if ((data & 0x00FF) != 0)
			return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

		dev->ascii_delimiter = (u8)(data >> 8);
		dev->diagnostic_register |= DIAG_REG_ASCII_DELIMITER_CHANGED;
		memcpy(resp, req, 5);
		return 5;
	case DIAG_SUB_LISTEN_ONLY_MODE:
		dev->listen_only = true;
		return 0;
	case DIAG_SUB_CLEAR_COUNTERS:
		clear_diagnostic_counters(dev);
		memcpy(resp, req, 5);
		return 5;
	case DIAG_SUB_RETURN_BUS_MESSAGE_COUNT:
	case DIAG_SUB_RETURN_SLAVE_MESSAGE_COUNT:
		return diagnostic_counter_response(resp, subfunction, dev->message_count);
	case DIAG_SUB_RETURN_BUS_COMM_ERROR_COUNT:
		return diagnostic_counter_response(resp, subfunction, dev->diag_comm_error_count);
	case DIAG_SUB_RETURN_BUS_EXCEPTION_ERROR_COUNT:
		return diagnostic_counter_response(resp, subfunction, dev->diag_exception_count);
	case DIAG_SUB_RETURN_SLAVE_NO_RESPONSE_COUNT:
		return diagnostic_counter_response(resp, subfunction, dev->diag_no_response_count);
	case DIAG_SUB_RETURN_SLAVE_NAK_COUNT:
		return diagnostic_counter_response(resp, subfunction, dev->diag_nak_count);
	case DIAG_SUB_RETURN_SLAVE_BUSY_COUNT:
		return diagnostic_counter_response(resp, subfunction, dev->diag_busy_count);
	case DIAG_SUB_RETURN_BUS_CHAR_OVERRUN_COUNT:
		return diagnostic_counter_response(resp, subfunction, dev->diag_overrun_count);
	case DIAG_SUB_CLEAR_OVERRUN_COUNTER_AND_FLAG:
		dev->diag_overrun_count = 0;
		dev->diagnostic_register &= (u16)~DIAG_REG_CHARACTER_OVERRUN;
		memcpy(resp, req, 5);
		return 5;
	default:
		return modbus_exception(resp, req[0], EXC_ILLEGAL_FUNCTION);
	}
}

static u32
get_com_event_counter(device_t* dev, const u8* req, u32 req_len, u8* resp)
{
	if (req_len != 1)
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

	resp[0] = req[0];
	put_be16(resp + 1, dev->com_status);
	put_be16(resp + 3, dev->com_event_count);
	return 5;
}

static u32
get_com_event_log(device_t* dev, const u8* req, u32 req_len, u8* resp)
{
	if (req_len != 1)
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

	resp[0] = req[0];
	resp[1] = (u8)(6 + dev->com_event_log_count);
	put_be16(resp + 2, dev->com_status);
	put_be16(resp + 4, dev->com_event_count);
	put_be16(resp + 6, dev->message_count);
	memcpy(resp + 8, dev->com_events, dev->com_event_log_count);
	return 8 + dev->com_event_log_count;
}

static u32
report_server_id(device_t* dev, const u8* req, u32 req_len, u8* resp)
{
	u32	additional_len = dev->server_id_additional_data_len;

	if (req_len != 1)
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

	resp[0] = req[0];
	resp[1] = (u8)(2 + additional_len);
	resp[2] = dev->server_id;
	resp[3] = dev->run_indicator;
	if (additional_len)
		memcpy(resp + 4, dev->server_id_additional_data, additional_len);
	return 4 + additional_len;
}

enum mei_type
{
	MEI_TYPE_READ_DEVICE_ID = 0x0E,
};

enum read_device_id_code
{
	READ_DEVICE_ID_BASIC    = 0x01,
	READ_DEVICE_ID_REGULAR  = 0x02,
	READ_DEVICE_ID_EXTENDED = 0x03,
	READ_DEVICE_ID_SPECIFIC = 0x04,
};

static const char*
extended_id_value(const device_t* dev, u8 id)
{
	for (u32 i = 0; i < dev->extended_id_count; i++)
	{
		if (dev->extended_id[i].id == id)
			return dev->extended_id[i].value;
	}
	return NULL;
}

static bool
next_extended_id(const device_t* dev, u32 from, u8* id, const char** value)
{
	u8		best = 0;
	const char*	found = NULL;

	for (u32 i = 0; i < dev->extended_id_count; i++)
	{
		u8 candidate = dev->extended_id[i].id;

		if (candidate >= from && (!found || candidate < best))
		{
			best  = candidate;
			found = dev->extended_id[i].value;
		}
	}
	if (!found)
		return false;

	*id    = best;
	*value = found;
	return true;
}

static bool
device_id_object_present(const device_t* dev, u8 id)
{
	if (id < DEVICE_ID_STANDARD_COUNT)
		return dev->device_id[id][0] != '\0';
	return extended_id_value(dev, id) != NULL;
}

static u32
read_device_identification(device_t* dev, const u8* req, u32 req_len, u8* resp)
{
	const char*	value = NULL;
	u8		mei = 0;
	u8		read_code = 0;
	u8		object_id = 0;
	u8		conformity = 0x81;
	u32		id = 0;
	u32		last = 0;
	u32		count = 0;
	u32		offset = 0;
	bool		more = false;
	u8		next = 0;

	if (req_len < 2)
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);
	if (req[1] != MEI_TYPE_READ_DEVICE_ID)
		return modbus_exception(resp, req[0], EXC_ILLEGAL_FUNCTION);
	if (req_len != 4)
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

	mei       = req[1];
	read_code = req[2];
	object_id = req[3];
	if (read_code < READ_DEVICE_ID_BASIC || read_code > READ_DEVICE_ID_SPECIFIC)
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

	for (u32 i = DEVICE_ID_VENDOR_URL; i < DEVICE_ID_STANDARD_COUNT; i++)
	{
		if (dev->device_id[i][0] != '\0')
		{
			conformity = 0x82;
			break;
		}
	}
	if (dev->extended_id_count > 0)
		conformity = 0x83;

	if (read_code == READ_DEVICE_ID_SPECIFIC)
	{
		if (object_id < DEVICE_ID_STANDARD_COUNT)
		{
			if (dev->device_id[object_id][0] == '\0')
				return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_ADDRESS);
			value = dev->device_id[object_id];
		}
		else
		{
			value = extended_id_value(dev, object_id);
			if (!value)
				return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_ADDRESS);
		}
		id   = object_id;
		last = object_id + 1;
	}
	else
	{
		/* Stream access: unknown object id restarts at object 0 (Modbus spec 6.21). */
		if (read_code == READ_DEVICE_ID_BASIC)
		{
			if (object_id > DEVICE_ID_MAJOR_MINOR_REVISION)
				object_id = 0;
			last = DEVICE_ID_MAJOR_MINOR_REVISION + 1;
		}
		else if (read_code == READ_DEVICE_ID_REGULAR)
		{
			if (object_id >= DEVICE_ID_STANDARD_COUNT ||
			    !device_id_object_present(dev, object_id))
				object_id = 0;
			last = DEVICE_ID_STANDARD_COUNT;
		}
		else
		{
			if (!device_id_object_present(dev, object_id))
				object_id = 0;
			last = 0x100;
		}
		id = object_id;
	}

	resp[0] = req[0];
	resp[1] = mei;
	resp[2] = read_code;
	resp[3] = conformity;
	resp[4] = 0x00; /* More Follows */
	resp[5] = 0x00; /* Next Object Id */
	resp[6] = 0x00; /* Number Of Objects */
	offset = 7;

	while (id < last)
	{
		u32	len     = 0;
		u32	next_id = id + 1;

		if (id < DEVICE_ID_STANDARD_COUNT)
		{
			value = dev->device_id[id];
			if (value[0] == '\0')
			{
				id = next_id;
				continue;
			}
		}
		else
		{
			u8		found_id = 0;
			const char*	found    = NULL;

			if (!next_extended_id(dev, id, &found_id, &found))
				break;
			id      = found_id;
			next_id = found_id + 1;
			value   = found;
		}

		len = (u32)strlen(value);
		if (len > DEVICE_ID_OBJECT_MAX)
			len = DEVICE_ID_OBJECT_MAX;
		if (offset + 2 + len > MODBUS_MAX_PDU)
		{
			more = true;
			next = (u8)id;
			break;
		}

		resp[offset++] = (u8)id;
		resp[offset++] = (u8)len;
		memcpy(resp + offset, value, len);
		offset += len;
		count++;
		id = next_id;
	}

	if (count == 0)
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_ADDRESS);

	resp[4] = more ? 0xFF : 0x00;
	resp[5] = next;
	resp[6] = (u8)count;
	return offset;
}

/* --- dispatch --- */

static u32
read_coils(device_t* dev, const u8* req, u32 req_len, u8* resp)
{
	return read_bits(dev, SPACE_COIL, req, req_len, resp);
}

static u32
read_discrete_inputs(device_t* dev, const u8* req, u32 req_len, u8* resp)
{
	return read_bits(dev, SPACE_DISCRETE, req, req_len, resp);
}

static u32
read_holding_registers(device_t* dev, const u8* req, u32 req_len, u8* resp)
{
	return read_registers(dev, SPACE_HOLDING, req, req_len, resp);
}

static u32
read_input_registers(device_t* dev, const u8* req, u32 req_len, u8* resp)
{
	return read_registers(dev, SPACE_INPUT, req, req_len, resp);
}

typedef u32 (*pdu_handler_t)(device_t*, const u8*, u32, u8*);

typedef struct pdu_entry_t pdu_entry_t;
struct pdu_entry_t
{
	u8		fc;
	pdu_handler_t	handle;
};

static const pdu_entry_t PDU_HANDLERS[] = {
	{ FC_READ_COILS,                     read_coils                     },
	{ FC_READ_DISCRETE_INPUTS,           read_discrete_inputs           },
	{ FC_READ_HOLDING_REGISTERS,         read_holding_registers         },
	{ FC_READ_INPUT_REGISTERS,           read_input_registers           },
	{ FC_WRITE_SINGLE_COIL,              write_single_coil              },
	{ FC_WRITE_SINGLE_REGISTER,          write_single_register          },
	{ FC_READ_EXCEPTION_STATUS,          read_exception_status          },
	{ FC_DIAGNOSTICS,                    diagnostics                    },
	{ FC_GET_COM_EVENT_COUNTER,          get_com_event_counter          },
	{ FC_GET_COM_EVENT_LOG,              get_com_event_log              },
	{ FC_REPORT_SERVER_ID,               report_server_id               },
	{ FC_WRITE_MULTIPLE_COILS,           write_multiple_coils           },
	{ FC_WRITE_MULTIPLE_REGISTERS,       write_multiple_registers       },
	{ FC_READ_WRITE_MULTIPLE_REGISTERS,  read_write_multiple_registers  },
	{ FC_READ_FILE_RECORD,               read_file_record               },
	{ FC_WRITE_FILE_RECORD,              write_file_record              },
	{ FC_MASK_WRITE_REGISTER,            mask_write_register            },
	{ FC_READ_FIFO_QUEUE,                read_fifo_queue                },
	{ FC_ENCAPSULATED_INTERFACE_TRANSPORT, read_device_identification   },
};

static pdu_handler_t
find_pdu_handler(u8 fc)
{
	for (u32 i = 0; i < ARRAY_COUNT(PDU_HANDLERS); i++)
	{
		if (PDU_HANDLERS[i].fc == fc)
			return PDU_HANDLERS[i].handle;
	}
	return NULL;
}

static bool
is_restart_request(const u8* req, u32 req_len)
{
	u16 data = 0;

	if (req_len != 5 ||
	    req[0] != FC_DIAGNOSTICS ||
	    get_be16(req + 1) != DIAG_SUB_RESTART_COMMUNICATIONS)
		return false;

	data = get_be16(req + 3);
	return data == 0x0000 || data == 0xFF00;
}

u32
modbus_handle_pdu(device_t* dev, u64 now_ms, const u8* req, u32 req_len, u8* resp)
{
	pdu_handler_t	handler = NULL;
	u32		resp_len = 0;
	u8		fc       = 0;

	/* Listen-only monitors the bus but takes no action except Restart
	 * Communications, and no response is sent for those requests either. */
	if (dev->listen_only && !is_restart_request(req, req_len))
		return 0;

	if (req_len == 0)
	{
		dev->diag_exception_count = (u16)(dev->diag_exception_count + 1);
		return modbus_exception(resp, 0, EXC_ILLEGAL_FUNCTION);
	}

	fc = req[0];
	dev->message_count = (u16)(dev->message_count + 1);
	device_tick(dev, now_ms);

	handler = find_pdu_handler(fc);
	if (!handler)
	{
		dev->diag_exception_count = (u16)(dev->diag_exception_count + 1);
		return modbus_exception(resp, fc, EXC_ILLEGAL_FUNCTION);
	}

	resp_len = handler(dev, req, req_len, resp);
	if (resp_len == 0)
		return 0;
	if ((resp[0] & 0x80) != 0)
	{
		dev->diag_exception_count = (u16)(dev->diag_exception_count + 1);
		if (resp[1] == EXC_DEVICE_BUSY)
			dev->diag_busy_count = (u16)(dev->diag_busy_count + 1);
		else if (resp[1] == EXC_DEVICE_NAK)
			dev->diag_nak_count = (u16)(dev->diag_nak_count + 1);
	}
	else if (!is_restart_request(req, req_len) &&
	         fc != FC_GET_COM_EVENT_COUNTER &&
	         fc != FC_GET_COM_EVENT_LOG)
		dev->com_event_count = (u16)(dev->com_event_count + 1);
	return resp_len;
}

u32
modbus_handle_request(device_t* dev, rng_t* rng, u64 now_ms,
		      const u8* req, u32 req_len, u8* resp,
		      fault_decision_t* decision)
{
	u32	len = modbus_handle_pdu(dev, now_ms, req, req_len, resp);
	u8	fc  = (req_len == 0) ? 0 : req[0];

	if (dev)
		*decision = fault_decide(&dev->fault, rng);
	else
		*decision = (fault_decision_t){ 0 };

	/* The request was already processed above; faults only alter the response. */
	if (decision->exception_code != 0)
		len = modbus_exception(resp, fc, decision->exception_code);
	if (decision->corrupt)
		resp[len > 1 ? 1 : 0] ^= 0xFF;
	if (len == 0 || (dev && dev->listen_only))
		decision->drop = true;
	if (dev && decision->drop)
		dev->diag_no_response_count = (u16)(dev->diag_no_response_count + 1);
	return len;
}
