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
		.tls_verify = true,
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
	o.byte_timeout_ms = options->byte_timeout_ms;
	o.tls_ca_file     = options->tls_ca_file;
	o.tls_cert_file   = options->tls_cert_file;
	o.tls_key_file    = options->tls_key_file;
	o.tls_server_name = options->tls_server_name;
	o.tls_verify      = options->tls_verify;
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
mogbus_client_request(mogbus_client_t* client, mogbus_request_t* request)
{
	u8		req[MODBUS_MAX_PDU];
	u8		pdu[MODBUS_MAX_PDU];
	u32		pdu_len = 0;
	mogbus_result_t	err;

	if (request)
		request->reply_len = 0;
	if (!client ||
	    !request)
		return MOGBUS_ERR_INVALID;
	if (request->function_code == 0 ||
	    request->function_code > 127 ||
	    request->data_len > sizeof(req) - 1 ||
	    (request->data_len && !request->data) ||
	    (request->reply_cap && !request->reply))
		return reject(client, "a request needs a function code 1-127 and at most 252 data bytes");
	req[0] = request->function_code;
	if (request->data_len)
		memcpy(req + 1, request->data, request->data_len);
	err = call(client, request->unit, req, (u32)request->data_len + 1, pdu, &pdu_len);
	if (err != MOGBUS_OK)
		return err;
	if (pdu_len == 0)
		return MOGBUS_OK;	/* broadcast */
	if (pdu_len - 1 > request->reply_cap)
	{
		client_set_error(impl(client), "reply of %u data bytes does not fit the %zu-byte buffer",
				 pdu_len - 1, request->reply_cap);
		return MOGBUS_ERR_FRAME;
	}
	if (pdu_len > 1)
		memcpy(request->reply, pdu + 1, pdu_len - 1);
	request->reply_len = pdu_len - 1;
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
	return mogbus_client_read_device_identification(client, unit, 1, 0, out, cap, out_len, NULL);
}

/* Walks `count` [id][length][value] objects at p; returns the bytes they take, or 0 when they do
 * not fit in `avail`; *last_id is the id of the last one. */
static u32
walk_objects(const u8* p, u32 avail, u32 count, u8* last_id)
{
	u32 used = 0;

	for (u32 i = 0; i < count; i++)
	{
		if (used + 2 > avail || used + 2 + p[used + 1] > avail)
			return 0;
		*last_id = p[used];
		used += 2u + p[used + 1];
	}
	return used;
}

mogbus_result_t
mogbus_client_read_device_identification(mogbus_client_t* client, uint8_t unit, uint8_t read_code,
					 uint8_t object_id, uint8_t* out, size_t cap, size_t* out_len,
					 uint8_t* conformity)
{
	size_t		total = 0;
	u8		level = 0;
	u8		next  = object_id;

	if (out_len)
		*out_len = 0;
	if (!client)
		return MOGBUS_ERR_INVALID;
	if (unit == 0 || !out_len || (cap && !out) || read_code < 1 || read_code > 4)
		return reject(client, "read the device identification (code 1-4) from a unit other than 0 with a length");

	/* Every round returns at least one object and there are at most 256. */
	for (u32 round = 0; round < 256; round++)
	{
		u8		req[4] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0E, read_code, next };
		u8		pdu[MODBUS_MAX_PDU];
		u32		pdu_len = 0;
		u8		last_id = 0;
		u32		used;
		mogbus_result_t	err = call(client, unit, req, sizeof(req), pdu, &pdu_len);

		if (err != MOGBUS_OK)
			return err;
		/* [2B][0E][code][conformity][more][next object][count] then the objects */
		if (pdu_len < 7 ||
		    pdu[1] != 0x0E ||
		    pdu[2] != read_code ||
		    (pdu[4] != 0x00 && pdu[4] != 0xFF) ||
		    pdu[6] == 0)
			return bad_reply(client, req[0]);
		used = walk_objects(pdu + 7, pdu_len - 7, pdu[6], &last_id);
		if (used == 0 ||
		    used != pdu_len - 7 ||
		    (pdu[4] == 0xFF && (read_code == 4 || pdu[5] <= last_id)))
			return bad_reply(client, req[0]);
		if (total + used > cap)
		{
			client_set_error(impl(client), "the identification objects (%zu bytes) do not fit the %zu-byte buffer",
					 total + used, cap);
			return MOGBUS_ERR_FRAME;
		}
		memcpy(out + total, pdu + 7, used);
		total += used;
		level  = pdu[3];
		if (pdu[4] == 0x00)
		{
			*out_len = total;
			if (conformity)
				*conformity = level;
			return MOGBUS_OK;
		}
		next = pdu[5];
	}
	return bad_reply(client, FC_ENCAPSULATED_INTERFACE_TRANSPORT);
}

mogbus_result_t
mogbus_client_read_exception_status(mogbus_client_t* client, uint8_t unit, uint8_t* status)
{
	static const u8	req[] = { FC_READ_EXCEPTION_STATUS };
	u8		pdu[MODBUS_MAX_PDU];
	u32		pdu_len = 0;
	mogbus_result_t	err;

	if (!client)
		return MOGBUS_ERR_INVALID;
	if (unit == 0 || !status)
		return reject(client, "read the exception status of a unit other than 0 into a buffer");
	err = call(client, unit, req, sizeof(req), pdu, &pdu_len);
	if (err != MOGBUS_OK)
		return err;
	if (pdu_len != 2)
		return bad_reply(client, req[0]);
	*status = pdu[1];
	return MOGBUS_OK;
}

mogbus_result_t
mogbus_client_diagnostics(mogbus_client_t* client, uint8_t unit, uint16_t sub_function, const uint8_t* data,
			  size_t data_len, uint8_t* out, size_t cap, size_t* out_len)
{
	u8		req[MODBUS_MAX_PDU] = { FC_DIAGNOSTICS };
	u8		pdu[MODBUS_MAX_PDU];
	u32		pdu_len = 0;
	mogbus_result_t	err;

	if (out_len)
		*out_len = 0;
	if (!client)
		return MOGBUS_ERR_INVALID;
	if (!out_len ||
	    (cap && !out) ||
	    data_len > sizeof(req) - 3 ||
	    (data_len && !data))
		return reject(client, "send at most %zu data bytes with a reply length", sizeof(req) - 3);
	put_be16(req + 1, sub_function);
	if (data_len)
		memcpy(req + 3, data, data_len);
	err = call(client, unit, req, 3 + (u32)data_len, pdu, &pdu_len);
	if (err != MOGBUS_OK)
		return err;
	if (pdu_len == 0)
		return MOGBUS_OK;	/* broadcast */
	if (pdu_len < 3 ||
	    get_be16(pdu + 1) != sub_function ||
	    (sub_function != 0 && pdu_len != 5))
		return bad_reply(client, req[0]);
	if (pdu_len - 3 > cap)
	{
		client_set_error(impl(client), "reply of %u data bytes does not fit the %zu-byte buffer",
				 pdu_len - 3, cap);
		return MOGBUS_ERR_FRAME;
	}
	if (pdu_len > 3)
		memcpy(out, pdu + 3, pdu_len - 3);
	*out_len = pdu_len - 3;
	return MOGBUS_OK;
}

mogbus_result_t
mogbus_client_get_comm_event_counter(mogbus_client_t* client, uint8_t unit, uint16_t* status,
				     uint16_t* event_count)
{
	static const u8	req[] = { FC_GET_COM_EVENT_COUNTER };
	u8		pdu[MODBUS_MAX_PDU];
	u32		pdu_len = 0;
	mogbus_result_t	err;

	if (!client)
		return MOGBUS_ERR_INVALID;
	if (unit == 0 || !status || !event_count)
		return reject(client, "read the event counter of a unit other than 0 into buffers");
	err = call(client, unit, req, sizeof(req), pdu, &pdu_len);
	if (err != MOGBUS_OK)
		return err;
	if (pdu_len != 5)
		return bad_reply(client, req[0]);
	*status      = get_be16(pdu + 1);
	*event_count = get_be16(pdu + 3);
	return MOGBUS_OK;
}

mogbus_result_t
mogbus_client_get_comm_event_log(mogbus_client_t* client, uint8_t unit, uint16_t* status,
				 uint16_t* event_count, uint16_t* message_count, uint8_t* events,
				 size_t cap, size_t* events_len)
{
	static const u8	req[] = { FC_GET_COM_EVENT_LOG };
	u8		pdu[MODBUS_MAX_PDU];
	u32		pdu_len = 0;
	mogbus_result_t	err;

	if (events_len)
		*events_len = 0;
	if (!client)
		return MOGBUS_ERR_INVALID;
	if (unit == 0 || !status || !event_count || !message_count || !events_len || (cap && !events))
		return reject(client, "read the event log of a unit other than 0 into buffers");
	err = call(client, unit, req, sizeof(req), pdu, &pdu_len);
	if (err != MOGBUS_OK)
		return err;
	/* [0C][byte count][status 2][events 2][messages 2][event bytes, at most 64] */
	if (pdu_len < 8 ||
	    pdu[1] < 6 ||
	    pdu[1] > 6 + 64 ||
	    pdu_len != 2u + pdu[1])
		return bad_reply(client, req[0]);
	if (pdu[1] - 6u > cap)
	{
		client_set_error(impl(client), "the event log (%u bytes) does not fit the %zu-byte buffer",
				 pdu[1] - 6u, cap);
		return MOGBUS_ERR_FRAME;
	}
	*status        = get_be16(pdu + 2);
	*event_count   = get_be16(pdu + 4);
	*message_count = get_be16(pdu + 6);
	if (pdu[1] > 6)
		memcpy(events, pdu + 8, pdu[1] - 6u);
	*events_len = pdu[1] - 6u;
	return MOGBUS_OK;
}

mogbus_result_t
mogbus_client_report_server_id(mogbus_client_t* client, uint8_t unit, uint8_t* out, size_t cap, size_t* out_len)
{
	static const u8	req[] = { FC_REPORT_SERVER_ID };
	u8		pdu[MODBUS_MAX_PDU];
	u32		pdu_len = 0;
	mogbus_result_t	err;

	if (out_len)
		*out_len = 0;
	if (!client)
		return MOGBUS_ERR_INVALID;
	if (unit == 0 || !out_len || (cap && !out))
		return reject(client, "read the server id of a unit other than 0 with a length");
	err = call(client, unit, req, sizeof(req), pdu, &pdu_len);
	if (err != MOGBUS_OK)
		return err;
	if (pdu_len < 2 || pdu_len != 2u + pdu[1])
		return bad_reply(client, req[0]);
	if (pdu[1] > cap)
	{
		client_set_error(impl(client), "the server id report (%u bytes) does not fit the %zu-byte buffer",
				 pdu[1], cap);
		return MOGBUS_ERR_FRAME;
	}
	if (pdu[1])
		memcpy(out, pdu + 2, pdu[1]);
	*out_len = pdu[1];
	return MOGBUS_OK;
}

/* One sub-request of FC 14/15 must name a record range that exists in the protocol. */
static bool
file_record_valid(const mogbus_client_file_record_t* r)
{
	return r->file >= 1 &&
	       r->length >= 1 &&
	       (u32)r->record + r->length - 1 <= 9999 &&
	       r->values;
}

mogbus_result_t
mogbus_client_read_file_records(mogbus_client_t* client, uint8_t unit, const mogbus_client_file_record_t* records,
				size_t count)
{
	u8		req[2 + 7 * 35] = { FC_READ_FILE_RECORD };
	u8		pdu[MODBUS_MAX_PDU];
	u32		pdu_len = 0;
	u32		used = 2;
	mogbus_result_t	err;

	if (!client)
		return MOGBUS_ERR_INVALID;
	if (unit == 0 || !records || count < 1 || count > 35)
		return reject(client, "read 1-35 file records from a unit other than 0");
	for (size_t i = 0; i < count; i++)
	{
		if (!file_record_valid(&records[i]))
			return reject(client, "file record %zu: file 1-65535, records 0-9999, length 1 or more, a buffer", i);
		req[used] = 0x06;
		put_be16(req + used + 1, records[i].file);
		put_be16(req + used + 3, records[i].record);
		put_be16(req + used + 5, records[i].length);
		used += 7;
	}
	req[1] = (u8)(used - 2);
	err = call(client, unit, req, used, pdu, &pdu_len);
	if (err != MOGBUS_OK)
		return err;
	/* [14][data length] then per sub-request [1 + 2 x length][06][values] */
	if (pdu_len < 2 || pdu_len != 2u + pdu[1])
		return bad_reply(client, req[0]);
	used = 2;
	for (size_t i = 0; i < count; i++)
	{
		u32 bytes = 2u * records[i].length;

		if (used + 2 + bytes > pdu_len ||
		    pdu[used] != 1 + bytes ||
		    pdu[used + 1] != 0x06)
			return bad_reply(client, req[0]);
		for (u32 v = 0; v < records[i].length; v++)
			records[i].values[v] = get_be16(pdu + used + 2 + 2 * v);
		used += 2 + bytes;
	}
	if (used != pdu_len)
		return bad_reply(client, req[0]);
	return MOGBUS_OK;
}

mogbus_result_t
mogbus_client_write_file_records(mogbus_client_t* client, uint8_t unit, const mogbus_client_file_record_t* records,
				 size_t count)
{
	u8	req[MODBUS_MAX_PDU] = { FC_WRITE_FILE_RECORD };
	u32	used = 2;

	if (!client)
		return MOGBUS_ERR_INVALID;
	if (!records || count < 1)
		return reject(client, "write at least one file record");
	for (size_t i = 0; i < count; i++)
	{
		if (!file_record_valid(&records[i]))
			return reject(client, "file record %zu: file 1-65535, records 0-9999, length 1 or more, a buffer", i);
		/* The request PDU holds 253 bytes: function code, byte count and at most 251 of records. */
		if (used + 7 + 2u * records[i].length > sizeof(req))
			return reject(client, "the file records do not fit one request (251 bytes)");
		req[used] = 0x06;
		put_be16(req + used + 1, records[i].file);
		put_be16(req + used + 3, records[i].record);
		put_be16(req + used + 5, records[i].length);
		for (u32 v = 0; v < records[i].length; v++)
			put_be16(req + used + 7 + 2 * v, records[i].values[v]);
		used += 7 + 2u * records[i].length;
	}
	req[1] = (u8)(used - 2);
	return write_echo(client, req, used, unit);
}

mogbus_result_t
mogbus_client_read_fifo(mogbus_client_t* client, uint8_t unit, uint16_t address, uint16_t* values, size_t cap,
			size_t* count)
{
	u8		req[3] = { FC_READ_FIFO_QUEUE };
	u8		pdu[MODBUS_MAX_PDU];
	u32		pdu_len = 0;
	u32		n;
	mogbus_result_t	err;

	if (count)
		*count = 0;
	if (!client)
		return MOGBUS_ERR_INVALID;
	if (unit == 0 || !count || (cap && !values))
		return reject(client, "read a FIFO of a unit other than 0 with a count");
	put_be16(req + 1, address);
	err = call(client, unit, req, sizeof(req), pdu, &pdu_len);
	if (err != MOGBUS_OK)
		return err;
	/* [18][byte count 2][FIFO count 2][values], 31 at most */
	if (pdu_len < 5)
		return bad_reply(client, req[0]);
	n = get_be16(pdu + 3);
	if (n > 31 ||
	    get_be16(pdu + 1) != 2 + 2 * n ||
	    pdu_len != 5 + 2 * n)
		return bad_reply(client, req[0]);
	if (n > cap)
	{
		client_set_error(impl(client), "the FIFO holds %u values, more than the %zu-value buffer", n, cap);
		return MOGBUS_ERR_FRAME;
	}
	for (u32 i = 0; i < n; i++)
		values[i] = get_be16(pdu + 5 + 2 * i);
	*count = n;
	return MOGBUS_OK;
}

mogbus_result_t
mogbus_client_canopen_request(mogbus_client_t* client, uint8_t unit, const uint8_t* request, size_t request_len,
			      uint8_t* reply, size_t cap, size_t* reply_len)
{
	u8		req[MODBUS_MAX_PDU] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0D };
	u8		pdu[MODBUS_MAX_PDU];
	u32		pdu_len = 0;
	mogbus_result_t	err;

	if (reply_len)
		*reply_len = 0;
	if (!client)
		return MOGBUS_ERR_INVALID;
	if (!request ||
	    request_len < 1 ||
	    request_len > sizeof(req) - 2 ||
	    !reply_len ||
	    (cap && !reply))
		return reject(client, "send 1-%zu request bytes with a reply length", sizeof(req) - 2);
	memcpy(req + 2, request, request_len);
	err = call(client, unit, req, 2 + (u32)request_len, pdu, &pdu_len);
	if (err == MOGBUS_ERR_EXCEPTION && pdu_len >= 2 && pdu_len - 1 <= cap)
	{
		memcpy(reply, pdu + 1, pdu_len - 1);
		*reply_len = pdu_len - 1;
	}
	if (err != MOGBUS_OK)
		return err;
	if (pdu_len == 0)
		return MOGBUS_OK;	/* broadcast */
	/* [2B][0D][control][00][network id][node][index 2][subindex][start 2][count 2][data] */
	if (pdu_len < 12 || pdu[1] != 0x0D || pdu[2] != request[0])
		return bad_reply(client, req[0]);
	if (pdu_len - 2 > cap)
	{
		client_set_error(impl(client), "reply of %u bytes does not fit the %zu-byte buffer", pdu_len - 2, cap);
		return MOGBUS_ERR_FRAME;
	}
	memcpy(reply, pdu + 2, pdu_len - 2);
	*reply_len = pdu_len - 2;
	return MOGBUS_OK;
}
