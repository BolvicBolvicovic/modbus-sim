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

/* --- dispatch --- */

u32
modbus_handle_pdu(device_t* dev, u64 now_ms, const u8* req, u32 req_len, u8* resp)
{
	if (req_len == 0)
		return modbus_exception(resp, 0, EXC_ILLEGAL_FUNCTION);

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
	case FC_WRITE_MULTIPLE_COILS:
		return write_multiple_coils(dev, req, req_len, resp);
	case FC_WRITE_MULTIPLE_REGISTERS:
		return write_multiple_registers(dev, req, req_len, resp);
	default:
		return modbus_exception(resp, req[0], EXC_ILLEGAL_FUNCTION);
	}
}
