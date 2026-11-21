#ifndef PDU_H
#define PDU_H

#include "vlib/c_types.h"
#include "device/device.h"

/* Supported function codes. */
enum modbus_function_code
{
	FC_READ_COILS                     = 0x01,
	FC_READ_DISCRETE_INPUTS           = 0x02,
	FC_READ_HOLDING_REGISTERS         = 0x03,
	FC_READ_INPUT_REGISTERS           = 0x04,
	FC_WRITE_SINGLE_COIL              = 0x05,
	FC_WRITE_SINGLE_REGISTER          = 0x06,
	FC_READ_EXCEPTION_STATUS          = 0x07,
	FC_DIAGNOSTICS                    = 0x08,
	FC_GET_COM_EVENT_COUNTER          = 0x0B,
	FC_GET_COM_EVENT_LOG              = 0x0C,
	FC_WRITE_MULTIPLE_COILS           = 0x0F,
	FC_WRITE_MULTIPLE_REGISTERS       = 0x10,
	FC_REPORT_SERVER_ID               = 0x11,
	FC_READ_FILE_RECORD               = 0x14,
	FC_WRITE_FILE_RECORD              = 0x15,
	FC_MASK_WRITE_REGISTER            = 0x16,
	FC_READ_WRITE_MULTIPLE_REGISTERS  = 0x17,
	FC_READ_FIFO_QUEUE                = 0x18,
	FC_ENCAPSULATED_INTERFACE_TRANSPORT = 0x2B,
};

/* Exception codes. */
enum modbus_exception_code
{
	EXC_ILLEGAL_FUNCTION      = 0x01,
	EXC_ILLEGAL_DATA_ADDRESS  = 0x02,
	EXC_ILLEGAL_DATA_VALUE    = 0x03,
	EXC_GATEWAY_TARGET_FAILED = 0x0B,
};

/* Protocol limits from the Modbus specification. */
#define MB_MAX_READ_BITS		2000
#define MB_MAX_READ_REGISTERS		125
#define MB_MAX_WRITE_BITS		1968
#define MB_MAX_WRITE_REGISTERS		123

/* Name: modbus_handle_pdu
 * Description: runs one request PDU against a device and writes the response PDU.
 * `resp` must hold MODBUS_MAX_PDU bytes. Returns the response length (always >= 2).
 * Protocol problems become Modbus exception responses; this never fails.
 * */
u32	modbus_handle_pdu(device_t* dev, u64 now_ms, const u8* req, u32 req_len, u8* resp);

/* Builds an exception response PDU. Returns its length (2). */
u32	modbus_exception(u8* resp, u8 function, u8 code);

#endif
