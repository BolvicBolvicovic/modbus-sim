#include "modbus/pdu.h"
#include "modbus/bytes.h"
#include "modbus/mbap.h"
#include <string.h>

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

	if (read_qty < 1 || read_qty > 0x007D || write_qty < 1 || write_qty > 0x0079)
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

static u32
read_file_record(device_t* dev, const u8* req, u32 req_len, u8* resp)
{
	u32	byte_count    = 0;
	u32	subreq_count  = 0;
	u32	resp_data_len = 0;
	u32	offset        = 0;

	if (req_len < 2)
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

	byte_count = req[1];
	if (byte_count < 0x07 || byte_count > 0xF5 || (byte_count % 7) != 0)
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

	if (req_len != 2 + byte_count)
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

	subreq_count = byte_count / 7;

	/* Shape/value checks (03): framing, reference type, ranges, response capacity. */
	for (u32 i = 0; i < subreq_count; i++)
	{
		const u8*	sr      = req + 2 + i * 7;
		u32		rec_no  = get_be16(sr + 3);
		u32		rec_len = get_be16(sr + 5);

		if (sr[0] != 0x06)
			return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

		if (rec_no > 0x270F)
			return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

		if (rec_len < 1)
			return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

		resp_data_len += 2 + rec_len * 2;
	}

	if (2 + resp_data_len > MODBUS_MAX_PDU)
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

	/* Availability checks before building the response (all-or-nothing). */
	for (u32 i = 0; i < subreq_count; i++)
	{
		u16		words[MODBUS_MAX_PDU / 2];
		const u8*	sr      = req + 2 + i * 7;
		u16		file_no = get_be16(sr + 1);
		u16		rec_no  = get_be16(sr + 3);
		u16		rec_len = get_be16(sr + 5);

		if (!device_read_file_records(dev, file_no, rec_no, rec_len, words))
			return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_ADDRESS);
	}

	resp[0] = req[0];
	resp[1] = (u8)resp_data_len;
	offset  = 2;

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
write_file_record(device_t* dev, const u8* req, u32 req_len, u8* resp)
{
	u32	byte_count = 0;
	u32	offset     = 0;

	if (req_len < 2)
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

	byte_count = req[1];
	if (byte_count < 0x09 || byte_count > 0xFB)
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

	if (req_len != 2 + byte_count)
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

	/* Shape/value checks and framing across variable-length sub-requests. */
	offset = 2;
	while (offset < 2 + byte_count)
	{
		const u8*	sr      = req + offset;
		u32		remain  = 2 + byte_count - offset;
		u32		rec_no  = 0;
		u32		rec_len = 0;
		u32		sr_len  = 0;

		if (remain < 7)
			return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

		rec_no  = get_be16(sr + 3);
		rec_len = get_be16(sr + 5);
		sr_len  = 7 + rec_len * 2;

		if (sr[0] != 0x06)
			return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

		if (rec_no > 0x270F || rec_no + rec_len - 1 > 0x270F)
			return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

		if (rec_len < 1 || sr_len > remain)
			return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

		offset += sr_len;
	}

	/* Availability/writability checks before applying any change. */
	offset = 2;
	while (offset < 2 + byte_count)
	{
		u16		words[MODBUS_MAX_PDU / 2];
		const u8*	sr      = req + offset;
		u16		file_no = get_be16(sr + 1);
		u16		rec_no  = get_be16(sr + 3);
		u16		rec_len = get_be16(sr + 5);

		if (!device_read_file_records(dev, file_no, rec_no, rec_len, words))
			return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_ADDRESS);

		offset += 7 + (u32)rec_len * 2;
	}

	/* Apply all writes (all-or-nothing validated above). */
	offset = 2;
	while (offset < 2 + byte_count)
	{
		const u8*	sr      = req + offset;
		u16		file_no = get_be16(sr + 1);
		u16		rec_no  = get_be16(sr + 3);
		u16		rec_len = get_be16(sr + 5);
		u16		values[MODBUS_MAX_PDU / 2];

		for (u32 i = 0; i < rec_len; i++)
			values[i] = get_be16(sr + 7 + i * 2);

		device_write_file_records(dev, file_no, rec_no, rec_len, values);
		offset += 7 + (u32)rec_len * 2;
	}

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

/* --- dispatch --- */

u32
modbus_handle_pdu(device_t* dev, u64 now_ms, const u8* req, u32 req_len, u8* resp)
{
	if (req_len == 0)
		return modbus_exception(resp, 0, EXC_ILLEGAL_FUNCTION);

	dev->com_event_count = (u16)(dev->com_event_count + 1);
	dev->message_count   = (u16)(dev->message_count + 1);
	device_tick(dev, now_ms);

	switch (req[0])
	{
	case FC_READ_COILS:
		return read_bits(dev, SPACE_COIL, req, req_len, resp);
	case FC_READ_DISCRETE_INPUTS:
		return read_bits(dev, SPACE_DISCRETE, req, req_len, resp);
	case FC_READ_HOLDING_REGISTERS:
		return read_registers(dev, SPACE_HOLDING, req, req_len, resp);
	case FC_READ_INPUT_REGISTERS:
		return read_registers(dev, SPACE_INPUT, req, req_len, resp);
	case FC_WRITE_SINGLE_COIL:
		return write_single_coil(dev, req, req_len, resp);
	case FC_WRITE_SINGLE_REGISTER:
		return write_single_register(dev, req, req_len, resp);
	case FC_READ_EXCEPTION_STATUS:
		if (req_len != 1)
			return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);
		resp[0] = req[0];
		resp[1] = dev->exception_status;
		return 2;
	case FC_DIAGNOSTICS:
		if (req_len != 5)
			return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);
		if (get_be16(req + 1) != 0x0000)
			return modbus_exception(resp, req[0], EXC_ILLEGAL_FUNCTION);
		memcpy(resp, req, 5);
		return 5;
	case FC_GET_COM_EVENT_COUNTER:
		if (req_len != 1)
			return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);
		resp[0] = req[0];
		put_be16(resp + 1, 0x0000);
		put_be16(resp + 3, dev->com_event_count);
		return 5;
	case FC_GET_COM_EVENT_LOG:
		if (req_len != 1)
			return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);
		resp[0] = req[0];
		resp[1] = 6; /* status + event count + message count, no events */
		put_be16(resp + 2, 0x0000);
		put_be16(resp + 4, dev->com_event_count);
		put_be16(resp + 6, dev->message_count);
		return 8;
	case FC_REPORT_SERVER_ID:
		if (req_len != 1)
			return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);
		resp[0] = req[0];
		resp[1] = 2; /* server ID + run indicator, no additional data */
		resp[2] = dev->server_id;
		resp[3] = dev->run_indicator;
		return 4;
	case FC_WRITE_MULTIPLE_COILS:
		return write_multiple_coils(dev, req, req_len, resp);
	case FC_WRITE_MULTIPLE_REGISTERS:
		return write_multiple_registers(dev, req, req_len, resp);
	case FC_READ_WRITE_MULTIPLE_REGISTERS:
		return read_write_multiple_registers(dev, req, req_len, resp);
	case FC_READ_FILE_RECORD:
		return read_file_record(dev, req, req_len, resp);
	case FC_WRITE_FILE_RECORD:
		return write_file_record(dev, req, req_len, resp);
	case FC_MASK_WRITE_REGISTER:
		return mask_write_register(dev, req, req_len, resp);
	case FC_READ_FIFO_QUEUE:
		return read_fifo_queue(dev, req, req_len, resp);
	default:
		return modbus_exception(resp, req[0], EXC_ILLEGAL_FUNCTION);
	}
}
