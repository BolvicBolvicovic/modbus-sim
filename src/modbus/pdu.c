#include "modbus/pdu.h"
#include "modbus/bytes.h"
#include "modbus/mbap.h"
#include "vlib/macros.h"
#include <string.h>

_Static_assert(MODBUS_MAX_PDU / 2 == DEVICE_MAX_RECORD_VALUES,
	       "file record value limit must match Modbus PDU capacity");

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

static bool
read_file_record_shape_ok(const u8* req, u32 req_len, u32* subreq_count, u32* resp_data_len)
{
	u32	byte_count = 0;

	if (req_len < 2)
		return false;

	byte_count = req[1];
	if (byte_count < 0x07 || byte_count > 0xF5 || (byte_count % 7) != 0)
		return false;
	if (req_len != 2 + byte_count)
		return false;

	*subreq_count  = byte_count / 7;
	*resp_data_len = 0;
	for (u32 i = 0; i < *subreq_count; i++)
	{
		const u8*	sr      = req + 2 + i * 7;
		u32		rec_no  = get_be16(sr + 3);
		u32		rec_len = get_be16(sr + 5);

		if (sr[0] != 0x06 || rec_no > 0x270F || rec_len < 1)
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
		u32		rec_no  = 0;
		u32		rec_len = 0;
		u32		sr_len  = 0;

		if (remain < 7)
			return false;

		rec_no  = get_be16(sr + 3);
		rec_len = get_be16(sr + 5);
		sr_len  = 7 + rec_len * 2;

		if (sr[0] != 0x06)
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

static u32
diagnostics(device_t* dev, const u8* req, u32 req_len, u8* resp)
{
	(void)dev;
	if (req_len != 5)
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

	if (get_be16(req + 1) != 0x0000)
		return modbus_exception(resp, req[0], EXC_ILLEGAL_FUNCTION);

	memcpy(resp, req, 5);
	return 5;
}

static u32
get_com_event_counter(device_t* dev, const u8* req, u32 req_len, u8* resp)
{
	if (req_len != 1)
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

	resp[0] = req[0];
	put_be16(resp + 1, 0x0000);
	put_be16(resp + 3, dev->com_event_count);
	return 5;
}

static u32
get_com_event_log(device_t* dev, const u8* req, u32 req_len, u8* resp)
{
	if (req_len != 1)
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

	resp[0] = req[0];
	resp[1] = 6; /* status + event count + message count, no events */
	put_be16(resp + 2, 0x0000);
	put_be16(resp + 4, dev->com_event_count);
	put_be16(resp + 6, dev->message_count);
	return 8;
}

static u32
report_server_id(device_t* dev, const u8* req, u32 req_len, u8* resp)
{
	if (req_len != 1)
		return modbus_exception(resp, req[0], EXC_ILLEGAL_DATA_VALUE);

	resp[0] = req[0];
	resp[1] = 2; /* server ID + run indicator, no additional data */
	resp[2] = dev->server_id;
	resp[3] = dev->run_indicator;
	return 4;
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

u32
modbus_handle_pdu(device_t* dev, u64 now_ms, const u8* req, u32 req_len, u8* resp)
{
	pdu_handler_t	handler = NULL;

	if (req_len == 0)
		return modbus_exception(resp, 0, EXC_ILLEGAL_FUNCTION);

	dev->com_event_count = (u16)(dev->com_event_count + 1);
	dev->message_count   = (u16)(dev->message_count + 1);
	device_tick(dev, now_ms);

	handler = find_pdu_handler(req[0]);
	if (!handler)
		return modbus_exception(resp, req[0], EXC_ILLEGAL_FUNCTION);

	return handler(dev, req, req_len, resp);
}
