#ifndef PDU_H
#define PDU_H

#include "vlib/c_types.h"
#include "device/device.h"

/* Supported function codes. */
#define FC_READ_COILS			0x01
#define FC_READ_DISCRETE_INPUTS		0x02
#define FC_READ_HOLDING_REGISTERS	0x03
#define FC_READ_INPUT_REGISTERS		0x04
#define FC_WRITE_SINGLE_COIL		0x05
#define FC_WRITE_SINGLE_REGISTER	0x06
#define FC_READ_EXCEPTION_STATUS	0x07
#define FC_DIAGNOSTICS			0x08
#define FC_GET_COM_EVENT_COUNTER	0x0B
#define FC_GET_COM_EVENT_LOG		0x0C
#define FC_WRITE_MULTIPLE_COILS		0x0F
#define FC_WRITE_MULTIPLE_REGISTERS	0x10
#define FC_REPORT_SERVER_ID		0x11

/* Exception codes. */
#define EXC_ILLEGAL_FUNCTION		0x01
#define EXC_ILLEGAL_DATA_ADDRESS	0x02
#define EXC_ILLEGAL_DATA_VALUE		0x03
#define EXC_GATEWAY_TARGET_FAILED	0x0B

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
