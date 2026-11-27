/**
 * @file
 * @brief Public C client API (include/mogbus.h, "Client"): function code encoding and reply
 *        checking over the client transport.
 */
#include "mogbus.h"
#include "client/client.h"
#include "modbus/bytes.h"
#include "modbus/mbap.h"
#include "modbus/pdu.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* The public handle is the transport's client. */
static client_t*
impl(mogbus_client_t* c)
{
	return (client_t*)c;
}

static mogbus_result_t
to_error(client_status_t st)
{
	switch (st)
	{
		case CLIENT_OK       : return MOGBUS_OK;
		case CLIENT_NOMEM    : return MOGBUS_ERR_NOMEM;
		case CLIENT_UNSUPPORTED: return MOGBUS_ERR_UNSUPPORTED;
		case CLIENT_IO       : return MOGBUS_ERR_IO;
		case CLIENT_TIMEOUT  : return MOGBUS_ERR_TIMEOUT;
		case CLIENT_EXCEPTION: return MOGBUS_ERR_EXCEPTION;
		case CLIENT_FRAME    : return MOGBUS_ERR_FRAME;
		default              : return MOGBUS_ERR_INVALID;
	}
}

mogbus_client_options_t
mogbus_client_options_defaults(void)
{
	return (mogbus_client_options_t){
		.mode       = MOGBUS_MODE_TCP,
		.host       = "127.0.0.1",
		.port       = 1502,
		.data_bits  = 8,
		.parity     = MOGBUS_PARITY_NONE,
		.stop_bits  = 1,
		.timeout_ms = 1000,
	};
}

mogbus_result_t
mogbus_client_open(const mogbus_client_options_t* options, mogbus_client_t** out)
{
	client_options_t o;

	*out = NULL;
	if (!options)
	{
		client_detached_errorf("options are required");
		return MOGBUS_ERR_INVALID;
	}
	switch (options->mode)
	{
		case MOGBUS_MODE_TCP         : o.mode = CLIENT_TCP; break;
		case MOGBUS_MODE_UDP         : o.mode = CLIENT_UDP; break;
		case MOGBUS_MODE_RTU_TCP     : o.mode = CLIENT_RTU_TCP; break;
		case MOGBUS_MODE_RTU_UDP     : o.mode = CLIENT_RTU_UDP; break;
		case MOGBUS_MODE_ASCII_TCP   : o.mode = CLIENT_ASCII_TCP; break;
		case MOGBUS_MODE_ASCII_UDP   : o.mode = CLIENT_ASCII_UDP; break;
		case MOGBUS_MODE_RTU_DEVICE  : o.mode = CLIENT_RTU_SERIAL; break;
		case MOGBUS_MODE_ASCII_DEVICE: o.mode = CLIENT_ASCII_SERIAL; break;
		default:
			client_detached_errorf("mode %d is not a client mode", (int)options->mode);
			return MOGBUS_ERR_INVALID;
	}
	o.host       = options->host;
	o.port       = options->port;
	o.path       = options->path;
	o.baud       = options->baud;
	o.data_bits  = options->data_bits;
	o.parity     = (parity_t)options->parity;
	o.stop_bits  = options->stop_bits;
	o.tls        = options->tls_enabled;
	o.timeout_ms = options->timeout_ms;
	o.retries    = options->retries;
	return to_error(client_new(&o, (client_t**)out));
}

void
mogbus_client_close(mogbus_client_t* client)
{
	client_free(impl(client));
}

const char*
mogbus_client_last_error(const mogbus_client_t* client)
{
	return client_error((const client_t*)client);
}

uint8_t
mogbus_client_last_exception(const mogbus_client_t* client)
{
	return client_exception((const client_t*)client);
}

mogbus_result_t
mogbus_client_connect(mogbus_client_t* client)
{
	return to_error(client_connect(impl(client)));
}

/* Sends req and leaves the whole reply PDU (function code first) in reply. */
static mogbus_result_t
call(mogbus_client_t* client, u8 unit, const u8* req, u32 req_len, u8* reply, u32* reply_len)
{
	return to_error(client_request(impl(client), unit, req, req_len, reply, MODBUS_MAX_PDU, reply_len));
}

/* A malformed or out-of-range call: the message goes to the client like any failure. */
static mogbus_result_t __attribute__((format(printf, 2, 3)))
reject(mogbus_client_t* client, const char* fmt, ...)
{
	char		text[128];
	va_list		ap;

	va_start(ap, fmt);
	vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	client_set_error(impl(client), "%s", text);
	return MOGBUS_ERR_INVALID;
}

static mogbus_result_t
bad_reply(mogbus_client_t* client, u8 fc)
{
	client_set_error(impl(client), "reply to function code 0x%02X does not match the request", fc);
	return MOGBUS_ERR_FRAME;
}

mogbus_result_t
mogbus_client_request(mogbus_client_t* client, uint8_t unit, uint8_t function_code,
		      const uint8_t* data, size_t data_len,
		      uint8_t* reply, size_t reply_cap, size_t* reply_len)
{
	u8		req[MODBUS_MAX_PDU];
	u8		pdu[MODBUS_MAX_PDU];
	u32		pdu_len = 0;
	mogbus_result_t	err;

	if (reply_len)
		*reply_len = 0;
	if (!client)
		return MOGBUS_ERR_INVALID;
	if (function_code == 0 ||
	    function_code > 127 ||
	    data_len > sizeof(req) - 1 ||
	    (data_len && !data) ||
	    !reply_len ||
	    (reply_cap && !reply))
		return reject(client, "a request needs a function code 1-127, at most 252 data bytes and a reply length");
	req[0] = function_code;
	if (data_len)
		memcpy(req + 1, data, data_len);
	err = call(client, unit, req, (u32)data_len + 1, pdu, &pdu_len);
	if (err != MOGBUS_OK)
		return err;
	if (pdu_len == 0)
		return MOGBUS_OK;	/* broadcast */
	if (pdu_len - 1 > reply_cap)
	{
		client_set_error(impl(client), "reply of %u data bytes does not fit the %zu-byte buffer",
				 pdu_len - 1, reply_cap);
		return MOGBUS_ERR_FRAME;
	}
	if (pdu_len > 1)
		memcpy(reply, pdu + 1, pdu_len - 1);
	*reply_len = pdu_len - 1;
	return MOGBUS_OK;
}

static mogbus_result_t
read_bits(mogbus_client_t* client, u8 fc, u8 unit, u16 address, u16 count, u8* values)
{
	u8		req[5] = { fc };
	u8		pdu[MODBUS_MAX_PDU];
	u32		pdu_len = 0;
	u32		bytes   = ((u32)count + 7) / 8;
	mogbus_result_t	err;

	if (!client)
		return MOGBUS_ERR_INVALID;
	if (unit == 0 || !values || count < 1 || count > MB_MAX_READ_BITS)
		return reject(client, "read 1-%d bits from a unit other than 0 into a buffer", MB_MAX_READ_BITS);
	put_be16(req + 1, address);
	put_be16(req + 3, count);
	err = call(client, unit, req, sizeof(req), pdu, &pdu_len);
	if (err != MOGBUS_OK)
		return err;
	if (pdu_len != 2 + bytes || pdu[1] != bytes)
		return bad_reply(client, fc);
	for (u32 i = 0; i < count; i++)
		values[i] = (pdu[2 + i / 8] >> (i % 8)) & 1;
	return MOGBUS_OK;
}

static mogbus_result_t
read_registers(mogbus_client_t* client, u8 fc, u8 unit, u16 address, u16 count, u16* values)
{
	u8		req[5] = { fc };
	u8		pdu[MODBUS_MAX_PDU];
	u32		pdu_len = 0;
	mogbus_result_t	err;

	if (!client)
		return MOGBUS_ERR_INVALID;
	if (unit == 0 || !values || count < 1 || count > MB_MAX_READ_REGISTERS)
		return reject(client, "read 1-%d registers from a unit other than 0 into a buffer",
			      MB_MAX_READ_REGISTERS);
	put_be16(req + 1, address);
	put_be16(req + 3, count);
	err = call(client, unit, req, sizeof(req), pdu, &pdu_len);
	if (err != MOGBUS_OK)
		return err;
	if (pdu_len != 2u + 2u * count || pdu[1] != 2 * count)
		return bad_reply(client, fc);
	for (u32 i = 0; i < count; i++)
		values[i] = get_be16(pdu + 2 + 2 * i);
	return MOGBUS_OK;
}

mogbus_result_t
mogbus_client_read_coils(mogbus_client_t* client, uint8_t unit, uint16_t address, uint16_t count, uint8_t* values)
{
	return read_bits(client, FC_READ_COILS, unit, address, count, values);
}

mogbus_result_t
mogbus_client_read_discrete_inputs(mogbus_client_t* client, uint8_t unit, uint16_t address, uint16_t count,
				   uint8_t* values)
{
	return read_bits(client, FC_READ_DISCRETE_INPUTS, unit, address, count, values);
}

mogbus_result_t
mogbus_client_read_holding_registers(mogbus_client_t* client, uint8_t unit, uint16_t address, uint16_t count,
				     uint16_t* values)
{
	return read_registers(client, FC_READ_HOLDING_REGISTERS, unit, address, count, values);
}

mogbus_result_t
mogbus_client_read_input_registers(mogbus_client_t* client, uint8_t unit, uint16_t address, uint16_t count,
				   uint16_t* values)
{
	return read_registers(client, FC_READ_INPUT_REGISTERS, unit, address, count, values);
}

/* FC 05, 06 and 16 send 4 or 6 data bytes and get the same bytes back. */
static mogbus_result_t
write_echo(mogbus_client_t* client, const u8* req, u32 req_len, u8 unit)
{
	u8		pdu[MODBUS_MAX_PDU];
	u32		pdu_len = 0;
	mogbus_result_t	err;

	if (!client)
		return MOGBUS_ERR_INVALID;
	err = call(client, unit, req, req_len, pdu, &pdu_len);
	if (err != MOGBUS_OK)
		return err;
	if (pdu_len != 0 && (pdu_len != req_len || memcmp(pdu, req, req_len) != 0))
		return bad_reply(client, req[0]);
	return MOGBUS_OK;
}

mogbus_result_t
mogbus_client_write_coil(mogbus_client_t* client, uint8_t unit, uint16_t address, bool on)
{
	u8 req[5] = { FC_WRITE_SINGLE_COIL };

	put_be16(req + 1, address);
	put_be16(req + 3, on ? 0xFF00 : 0x0000);
	return write_echo(client, req, sizeof(req), unit);
}

mogbus_result_t
mogbus_client_write_register(mogbus_client_t* client, uint8_t unit, uint16_t address, uint16_t value)
{
	u8 req[5] = { FC_WRITE_SINGLE_REGISTER };

	put_be16(req + 1, address);
	put_be16(req + 3, value);
	return write_echo(client, req, sizeof(req), unit);
}

mogbus_result_t
mogbus_client_mask_write_register(mogbus_client_t* client, uint8_t unit, uint16_t address, uint16_t and_mask,
				  uint16_t or_mask)
{
	u8 req[7] = { FC_MASK_WRITE_REGISTER };

	put_be16(req + 1, address);
	put_be16(req + 3, and_mask);
	put_be16(req + 5, or_mask);
	return write_echo(client, req, sizeof(req), unit);
}

/* FC 15 and 16: address and count echoed back. */
static mogbus_result_t
write_multiple(mogbus_client_t* client, u8 unit, const u8* req, u32 req_len)
{
	u8		pdu[MODBUS_MAX_PDU];
	u32		pdu_len = 0;
	mogbus_result_t	err = call(client, unit, req, req_len, pdu, &pdu_len);

	if (err != MOGBUS_OK)
		return err;
	if (pdu_len != 0 && (pdu_len != 5 || memcmp(pdu, req, 5) != 0))
		return bad_reply(client, req[0]);
	return MOGBUS_OK;
}

mogbus_result_t
mogbus_client_write_coils(mogbus_client_t* client, uint8_t unit, uint16_t address, uint16_t count,
			  const uint8_t* values)
{
	u8	req[6 + (MB_MAX_WRITE_BITS + 7) / 8] = { FC_WRITE_MULTIPLE_COILS };
	u32	bytes = ((u32)count + 7) / 8;

	if (!client)
		return MOGBUS_ERR_INVALID;
	if (!values || count < 1 || count > MB_MAX_WRITE_BITS)
		return reject(client, "write 1-%d coils from a buffer", MB_MAX_WRITE_BITS);
	put_be16(req + 1, address);
	put_be16(req + 3, count);
	req[5] = (u8)bytes;
	for (u32 i = 0; i < count; i++)
		if (values[i])
			req[6 + i / 8] |= (u8)(1u << (i % 8));
	return write_multiple(client, unit, req, 6 + bytes);
}

mogbus_result_t
mogbus_client_write_registers(mogbus_client_t* client, uint8_t unit, uint16_t address, uint16_t count,
			      const uint16_t* values)
{
	u8 req[6 + 2 * MB_MAX_WRITE_REGISTERS] = { FC_WRITE_MULTIPLE_REGISTERS };

	if (!client)
		return MOGBUS_ERR_INVALID;
	if (!values || count < 1 || count > MB_MAX_WRITE_REGISTERS)
		return reject(client, "write 1-%d registers from a buffer", MB_MAX_WRITE_REGISTERS);
	put_be16(req + 1, address);
	put_be16(req + 3, count);
	req[5] = (u8)(2 * count);
	for (u32 i = 0; i < count; i++)
		put_be16(req + 6 + 2 * i, values[i]);
	return write_multiple(client, unit, req, 6 + 2u * count);
}

mogbus_result_t
mogbus_client_read_write_registers(mogbus_client_t* client, uint8_t unit, uint16_t read_address,
				   uint16_t read_count, uint16_t* read_values, uint16_t write_address,
				   uint16_t write_count, const uint16_t* write_values)
{
	u8		req[10 + 2 * 121] = { FC_READ_WRITE_MULTIPLE_REGISTERS };
	u8		pdu[MODBUS_MAX_PDU];
	u32		pdu_len = 0;
	mogbus_result_t	err;

	if (!client)
		return MOGBUS_ERR_INVALID;
	if (unit == 0 ||
	    !read_values ||
	    !write_values ||
	    read_count < 1 ||
	    read_count > MB_MAX_READ_REGISTERS ||
	    write_count < 1 ||
	    write_count > 121)
		return reject(client, "read 1-%d and write 1-121 registers, from a unit other than 0, with buffers",
			      MB_MAX_READ_REGISTERS);
	put_be16(req + 1, read_address);
	put_be16(req + 3, read_count);
	put_be16(req + 5, write_address);
	put_be16(req + 7, write_count);
	req[9] = (u8)(2 * write_count);
	for (u32 i = 0; i < write_count; i++)
		put_be16(req + 10 + 2 * i, write_values[i]);
	err = call(client, unit, req, 10 + 2u * write_count, pdu, &pdu_len);
	if (err != MOGBUS_OK)
		return err;
	if (pdu_len != 2u + 2u * read_count || pdu[1] != 2 * read_count)
		return bad_reply(client, req[0]);
	for (u32 i = 0; i < read_count; i++)
		read_values[i] = get_be16(pdu + 2 + 2 * i);
	return MOGBUS_OK;
}

mogbus_result_t
mogbus_client_read_device_id(mogbus_client_t* client, uint8_t unit, uint8_t* out, size_t cap, size_t* out_len)
{
	/* MEI type 14, read device id code 1 (basic), first object 0. */
	static const u8	req[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0E, 0x01, 0x00 };
	u8		pdu[MODBUS_MAX_PDU];
	u32		pdu_len = 0;
	mogbus_result_t	err;

	if (out_len)
		*out_len = 0;
	if (!client)
		return MOGBUS_ERR_INVALID;
	if (unit == 0 || !out_len || (cap && !out))
		return reject(client, "read the device identification from a unit other than 0 with a length");
	err = call(client, unit, req, sizeof(req), pdu, &pdu_len);
	if (err != MOGBUS_OK)
		return err;
	/* [2B][0E][code][conformity][more][next object][count] then the objects */
	if (pdu_len < 7 || pdu[1] != 0x0E)
		return bad_reply(client, req[0]);
	if (pdu_len - 7 > cap)
	{
		client_set_error(impl(client), "the identification objects (%u bytes) do not fit the %zu-byte buffer",
				 pdu_len - 7, cap);
		return MOGBUS_ERR_FRAME;
	}
	memcpy(out, pdu + 7, pdu_len - 7);
	*out_len = pdu_len - 7;
	return MOGBUS_OK;
}
